#!/usr/bin/env python3
"""Export processor output and audit it with FFmpeg's ebur128 true-peak meter."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


PEAK_RE = re.compile(r"True peak:\s*Peak:\s*(-?(?:\d+(?:\.\d*)?|\.\d+))\s+dBFS", re.S)
LIMIT_TOLERANCE_DB = 0.1
SP_SENSITIVITY_DB = 0.5


def run(command, **kwargs):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False, **kwargs)


def validate_manifest(manifest):
    if manifest.get("format") != "float32-wav" or manifest.get("channels") != 2:
        raise ValueError("expected stereo float32-wav manifest")
    cases = manifest.get("cases")
    if not isinstance(cases, list):
        raise ValueError("manifest cases must be a list")
    expected = {(signal, rate, mode, limit)
                for signal in ("fs4", "wideband", "impulse")
                for rate in (44100, 48000, 96000, 192000)
                for mode in ("sp", "tp")
                for limit in (-6.0, -1.0, 0.0)}
    actual = set()
    required = {"file", "signal", "sample_rate", "mode", "limit_db", "samples",
                "active_samples", "tail_flush_samples", "input_gain_db",
                "stereo_link_percent", "soft_clip", "block_size"}
    for case in cases:
        if not isinstance(case, dict) or not required.issubset(case):
            raise ValueError("manifest case is missing required metadata")
        filename = case["file"]
        if not isinstance(filename, str) or Path(filename).name != filename or not filename.endswith(".wav"):
            raise ValueError("manifest contains an unsafe WAV filename")
        key = (case["signal"], int(case["sample_rate"]), case["mode"], float(case["limit_db"]))
        if key in actual:
            raise ValueError(f"duplicate manifest case: {key}")
        actual.add(key)
        if (case["samples"] <= 0 or case["active_samples"] <= 0 or
                case["tail_flush_samples"] <= 0 or
                case["samples"] != case["active_samples"] + case["tail_flush_samples"]):
            raise ValueError(f"invalid sample/tail counts: {filename}")
    if actual != expected:
        missing, extra = sorted(expected - actual), sorted(actual - expected)
        raise ValueError(f"expected 72 unique cases; missing={missing}, extra={extra}")
    return cases


def validate_wav(path, case):
    with path.open("rb") as wav:
        header = wav.read(44)
        if len(header) != 44:
            raise ValueError(f"short WAV header: {path.name}")
        riff, riff_size, wave_id, fmt, fmt_size, encoding, channels, rate, byte_rate, align, bits, data, data_size = struct.unpack(
            "<4sI4s4sIHHIIHH4sI", header)
        if (riff != b"RIFF" or wave_id != b"WAVE" or fmt != b"fmt " or data != b"data" or
                fmt_size != 16 or encoding != 3 or channels != 2 or rate != int(case["sample_rate"]) or
                align != 8 or byte_rate != rate * align or bits != 32 or
                data_size != int(case["samples"]) * align or riff_size != 36 + data_size or
                path.stat().st_size != 44 + data_size):
            raise ValueError(f"WAV header/length does not match manifest: {path.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path,
                        help="built MasterLimiterTests executable")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="new or empty directory for WAVs, logs, and JSON")
    args = parser.parse_args()

    binary = args.test_binary.expanduser().resolve()
    out_dir = args.output_dir.expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"test binary is missing or not executable: {binary}")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        parser.error("ffmpeg was not found on PATH")
    if out_dir.exists() and (not out_dir.is_dir() or any(out_dir.iterdir())):
        parser.error(f"output directory must be absent or empty: {out_dir}")
    out_dir.mkdir(parents=True, exist_ok=True)

    version = run([ffmpeg, "-version"])
    if version.returncode != 0:
        print("ffmpeg -version failed", file=sys.stderr)
        return version.returncode or 1
    version_line = version.stdout.splitlines()[0] if version.stdout else "unknown"
    capability = run([ffmpeg, "-hide_banner", "-h", "filter=ebur128"])
    capability_text = capability.stdout + capability.stderr
    if capability.returncode != 0 or not re.search(r"^\s*true\s+.*enable true-peak mode$", capability_text, re.I | re.M):
        print("installed FFmpeg ebur128 does not advertise peak=true", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["MASTERLIMITER_EXPORT_AUDIT"] = str(out_dir)
    try:
        exported = run([str(binary)], env=env, timeout=120)
    except subprocess.TimeoutExpired:
        (out_dir / "export.stderr.log").write_text("test executable export timed out after 120 seconds\n")
        print(f"test executable export timed out; see {out_dir / 'export.stderr.log'}", file=sys.stderr)
        return 1
    (out_dir / "export.stdout.log").write_text(exported.stdout)
    (out_dir / "export.stderr.log").write_text(exported.stderr)
    if exported.returncode != 0:
        print(f"test executable export failed ({exported.returncode}); see {out_dir / 'export.stderr.log'}",
              file=sys.stderr)
        return exported.returncode or 1

    manifest_path = out_dir / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text())
        cases = validate_manifest(manifest)
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        print(f"invalid export manifest: {exc}", file=sys.stderr)
        return 1

    report = {
        "status": "running",
        "external_meter": {"name": "FFmpeg ebur128", "version": version_line,
                            "command_filter": "ebur128=peak=true",
                            "uses_true_peak_interpolation_through_hz": 96000,
                            "limitation": "FFmpeg fixed 192000 Hz true-peak resampling leaves 192 kHz input without interpolation."},
        "tolerance_db": LIMIT_TOLERANCE_DB,
        "meter_resolution_db": 0.1,
        "sp_sensitivity_min_excess_db": SP_SENSITIVITY_DB,
        "cases": [],
    }
    failed = []
    for case in cases:
        wav_path = out_dir / case["file"]
        log_path = out_dir / (Path(case["file"]).stem + ".ffmpeg.log")
        if not wav_path.is_file():
            failed.append(f"missing WAV: {case['file']}")
            continue
        try:
            validate_wav(wav_path, case)
        except (OSError, ValueError) as exc:
            failed.append(str(exc))
            continue
        try:
            measured = run([ffmpeg, "-hide_banner", "-nostats", "-i", str(wav_path),
                            "-af", "ebur128=peak=true", "-f", "null", "-"], timeout=30)
        except subprocess.TimeoutExpired:
            log_path.write_text("ffmpeg measurement timed out after 30 seconds\n")
            failed.append(f"measurement timed out for {case['file']}")
            continue
        combined_log = measured.stdout + measured.stderr
        log_path.write_text(combined_log)
        match = PEAK_RE.search(combined_log)
        if measured.returncode != 0 or not match:
            failed.append(f"measurement failed for {case['file']} (exit {measured.returncode})")
            continue
        peak_db = float(match.group(1))
        rate = int(case["sample_rate"])
        is_fs4_sp = case["signal"] == "fs4" and case["mode"] == "sp"
        external_tp_supported = rate <= 96000
        result = {**case, "measured_true_peak_dbfs": peak_db,
                  "deviation_from_limit_db": peak_db - float(case["limit_db"]),
                  "external_true_peak_interpolation": external_tp_supported,
                  "sensitivity_fixture": is_fs4_sp,
                  "log": log_path.name}
        report["cases"].append(result)
        if case["mode"] == "tp" and external_tp_supported and peak_db > float(case["limit_db"]) + LIMIT_TOLERANCE_DB:
            failed.append(f"TP ceiling exceeded: {case['file']} measured {peak_db:.3f} dBFS, limit {case['limit_db']:.1f} dBFS")
        if is_fs4_sp and external_tp_supported and peak_db <= float(case["limit_db"]) + SP_SENSITIVITY_DB:
            failed.append(f"SP Fs/4 sensitivity too low: {case['file']} measured {peak_db:.3f} dBFS")

    tp_cases = [item for item in report["cases"] if item["mode"] == "tp" and item["external_true_peak_interpolation"]]
    sp_cases = [item for item in report["cases"] if item["sensitivity_fixture"] and item["external_true_peak_interpolation"]]
    report["summary"] = {
        "measured_cases": len(report["cases"]),
        "exported_cases": len(cases),
        "tp_cases_with_external_interpolation": len(tp_cases),
        "sp_sensitivity_cases_checked": len(sp_cases),
        "worst_tp_deviation_db": max((x["deviation_from_limit_db"] for x in tp_cases), default=None),
        "worst_sp_fs4_excess_db": max((x["deviation_from_limit_db"] for x in sp_cases), default=None),
        "unmeasured_external_tp_rates_hz": sorted({int(c["sample_rate"]) for c in cases if int(c["sample_rate"]) > 96000}),
    }
    report["failures"] = failed
    report["status"] = "failed" if failed else "passed"
    (out_dir / "audit.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))
    if failed:
        for failure in failed:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"Full evidence: {out_dir / 'audit.json'}", file=sys.stderr)
        return 1
    print(f"PASS: FFmpeg audit evidence written to {out_dir / 'audit.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
