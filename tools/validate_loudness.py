#!/usr/bin/env python3
"""Compare LoudnessMeter fixtures with FFmpeg ebur128 M/S/I output.

Requires Python 3.9+, FFmpeg with the ebur128 audio filter, and a built
MasterLimiterLoudnessTests executable. Writes fixtures, raw samples, command
logs, and a JSON report into an absent or empty output directory.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

TOLERANCE_LU = 0.1
META_RE = re.compile(r"lavfi\.r128\.(M|S|I)=(-?inf|-?[\d.]+)", re.I)


def run(command, timeout):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False, timeout=timeout)


def fixture(name, rate=48000):
    channels = 1 if name == "mono" else 2
    duration = 12 if name in ("loud", "absolute_gate", "relative_gate") else 8
    frequency = {"low": 100, "high": 10000}.get(name, 1000)
    samples = []
    for i in range(rate * duration):
        t = i / rate
        amp = 0.2
        if name in ("absolute_gate", "relative_gate") and t >= duration / 2:
            amp = 0.0001 if name == "absolute_gate" else 0.02
        x = amp * math.sin(2 * math.pi * frequency * t)
        if channels == 1:
            samples.append(x)
        elif name == "antiphase":
            samples.extend((x, -x))
        else:
            samples.extend((x, x))
    return rate, channels, samples


def write_files(out_dir, case_id, rate, channels, samples):
    raw = out_dir / f"{case_id}.f32"
    wav = out_dir / f"{case_id}.wav"
    packed = struct.pack("<" + "f" * len(samples), *samples)
    raw.write_bytes(packed)
    data_size = len(packed)
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    wav.write_bytes(b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", data_size) + packed)
    return raw, wav


def parse_ffmpeg(text):
    values = {"M": [], "S": [], "I": []}
    for key, value in META_RE.findall(text):
        values[key.upper()].append(-100.0 if "inf" in value.lower() else float(value))
    if not all(values.values()):
        return None
    return {"momentary": values["M"][-1], "short_term": values["S"][-1], "integrated": values["I"][-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", required=True, type=Path,
                        help="built MasterLimiterLoudnessTests executable")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="absent or empty directory for fixtures, logs, and JSON")
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
    report = {"status": "running", "ffmpeg": None, "tolerance_lu": TOLERANCE_LU, "cases": [], "failures": []}
    version = run([ffmpeg, "-version"], 10)
    if version.returncode:
        print("ffmpeg -version failed", file=sys.stderr)
        return version.returncode or 1
    report["ffmpeg"] = version.stdout.splitlines()[0] if version.stdout else "unknown"
    cases = [(name, rate) for name in ("low", "loud", "high")
             for rate in (44100, 48000, 96000, 192000)]
    cases.extend((name, 48000) for name in ("absolute_gate", "relative_gate", "antiphase", "mono"))
    for name, rate in cases:
        case_id = f"{name}-{rate}"
        rate, channels, samples = fixture(name, rate)
        raw, wav = write_files(out_dir, case_id, rate, channels, samples)
        try:
            internal = run([str(binary), "--read-f32", str(raw), str(rate), str(channels)], 30)
            external = run([ffmpeg, "-hide_banner", "-nostats", "-i", str(wav),
                            "-af", "ebur128=metadata=1,ametadata=print", "-f", "null", "-"], 45)
        except subprocess.TimeoutExpired as exc:
            log = out_dir / f"{case_id}.timeout.log"
            log.write_text(f"command timed out: {exc.cmd}\n")
            report["failures"].append(f"{case_id}: command timed out")
            continue
        (out_dir / f"{case_id}.internal.log").write_text(internal.stdout + internal.stderr)
        ffmpeg_log = external.stdout + external.stderr
        (out_dir / f"{case_id}.ffmpeg.log").write_text(ffmpeg_log)
        try:
            ours = json.loads(internal.stdout)
        except json.JSONDecodeError:
            report["failures"].append(f"{case_id}: invalid internal meter response (exit {internal.returncode})")
            continue
        reference = parse_ffmpeg(ffmpeg_log)
        if internal.returncode or external.returncode or reference is None:
            report["failures"].append(f"{case_id}: meter command failed or FFmpeg values were not found (binary={internal.returncode}, ffmpeg={external.returncode})")
            continue
        case = {"fixture": name, "sample_rate": rate, "internal": ours, "ffmpeg": reference, "deviation_lu": {}}
        for metric, external_key in (("momentary", "momentary"), ("short_term", "short_term"), ("integrated", "integrated")):
            delta = float(ours[metric if metric != "short_term" else "short_term"]) - float(reference[external_key])
            case["deviation_lu"][metric] = delta
            if abs(delta) > TOLERANCE_LU:
                report["failures"].append(f"{case_id}: {metric} differs by {delta:.3f} LU (limit {TOLERANCE_LU:.2f})")
        report["cases"].append(case)
    report["status"] = "failed" if report["failures"] else "passed"
    report_path = out_dir / "audit.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    if report["failures"]:
        for failure in report["failures"]:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"Evidence: {report_path}", file=sys.stderr)
        return 1
    print(f"PASS: FFmpeg M/S/I comparison; evidence: {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
