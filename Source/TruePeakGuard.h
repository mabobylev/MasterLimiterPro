#pragma once
#include <juce_dsp/juce_dsp.h>
#include "LookaheadLimiter.h"

// A second, 8x FIR-oversampled peak guard after the existing 20 ms limiter.
// Its latency is also applied to the SP and bypass paths, making mode changes
// safe for host compensation. 0.75 dB internal allowance covers reconstruction
// and downsampling; verified against an independent 16x output reconstruction.
class TruePeakGuard
{
public:
    static constexpr int chunk = 128;
    struct Result { float left, right, reduction; };
    void prepare(double sr)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(2, 3,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
        oversampler->initProcessing(chunk);
        const int filterDelay = juce::roundToInt(oversampler->getLatencyInSamples());
        guardDelay = juce::jmax(1, juce::roundToInt(sr * .001));
        latency = filterDelay + guardDelay;
        limiter.prepare(sr * 8.0, guardDelay * 8);
        scratch.setSize(2, chunk);
        const int size = latency + 1;
        for (auto& channel : aligned) channel.assign(static_cast<size_t>(size), 0.f);
        alignedGR.assign(static_cast<size_t>(size), 0.f);
        // Calibrate the downsampler delay with an impulse, before processing.
        scratch.clear();
        auto block = juce::dsp::AudioBlock<float>(scratch);
        auto high = oversampler->processSamplesUp(block);
        high.clear(); high.setSample(0, 0, 1.f);
        oversampler->processSamplesDown(block);
        int downDelay = 0; float max = 0;
        for (int i = 0; i < chunk; ++i)
            if (std::abs(scratch.getSample(0, i)) > max) { max = std::abs(scratch.getSample(0, i)); downDelay = i; }
        delayedGR.assign(static_cast<size_t>(downDelay + 1), 0.f);
        reset();
    }
    int getLatency() const noexcept { return latency; }
    void reset() noexcept
    {
        if (oversampler) oversampler->reset();
        limiter.reset(); scratch.clear();
        for (auto& channel : aligned) std::fill(channel.begin(), channel.end(), 0.f);
        std::fill(alignedGR.begin(), alignedGR.end(), 0.f);
        std::fill(delayedGR.begin(), delayedGR.end(), 0.f);
        index = grIndex = 0; active = false; warmup = 0;
        mix.setCurrentAndTargetValue(0.f);
    }
    void start(bool enabled, double sr)
    {
        mix.reset(sr, .020); mix.setCurrentAndTargetValue(enabled ? 1.f : 0.f);
        active = enabled;
        warmupLength = juce::roundToInt(sr * .030);
        releaseCoefficient = std::exp(-1.f / static_cast<float>(sr * 8.0 * .050));
    }
    // input/output scratch data is bounded by chunk; ceiling is per base sample.
    // Both normal and dry paths, plus main gain reduction, are aligned here.
    void process(float* left, float* right, const float* dryL, const float* dryR,
                 const float* ceiling, const float* links, const float* mainGR, int count,
                 bool enabled, bool bypassed) noexcept
    {
        if (enabled && !active) { active = true; warmup = warmupLength; }
        if (!enabled) mix.setTargetValue(0.f);
        const bool run = active;
        if (run)
        {
            for (int i = 0; i < count; ++i) { scratch.setSample(0, i, left[i]); scratch.setSample(1, i, right[i]); }
            auto block = juce::dsp::AudioBlock<float>(scratch).getSubBlock(0, static_cast<size_t>(count));
            auto high = oversampler->processSamplesUp(block);
            for (size_t i = 0; i < high.getNumSamples(); ++i)
            {
                const int base = static_cast<int>(i / 8);
                const auto f = limiter.process(high.getSample(0, static_cast<int>(i)), high.getSample(1, static_cast<int>(i)),
                    guardDelay * 8, ceiling[base] * .917275935f, releaseCoefficient, links[base]);
                high.setSample(0, static_cast<int>(i), f.audio[0]); high.setSample(1, static_cast<int>(i), f.audio[1]);
                if (i % 8 == 0) extraGR[static_cast<size_t>(base)] = f.reduction;
                else extraGR[static_cast<size_t>(base)] = juce::jmax(extraGR[static_cast<size_t>(base)], f.reduction);
            }
            oversampler->processSamplesDown(block);
        }
        for (int i = 0; i < count; ++i)
        {
            const int next = (index + 1) % static_cast<int>(alignedGR.size());
            aligned[0][static_cast<size_t>(index)] = left[i]; aligned[1][static_cast<size_t>(index)] = right[i];
            aligned[2][static_cast<size_t>(index)] = dryL[i]; aligned[3][static_cast<size_t>(index)] = dryR[i];
            alignedGR[static_cast<size_t>(index)] = mainGR[i];
            if (enabled)
            {
                if (warmup > 0) --warmup;
                else mix.setTargetValue(1.f);
            }
            const float amount = mix.getNextValue();
            const float baseL = aligned[0][static_cast<size_t>(next)], baseR = aligned[1][static_cast<size_t>(next)];
            const float tpL = run ? scratch.getSample(0, i) : baseL, tpR = run ? scratch.getSample(1, i) : baseR;
            left[i] = bypassed ? aligned[2][static_cast<size_t>(next)] : juce::jlimit(-ceiling[i], ceiling[i], baseL + amount * (tpL - baseL));
            right[i] = bypassed ? aligned[3][static_cast<size_t>(next)] : juce::jlimit(-ceiling[i], ceiling[i], baseR + amount * (tpR - baseR));
            const int grNext = (grIndex + 1) % static_cast<int>(delayedGR.size());
            delayedGR[static_cast<size_t>(grIndex)] = run ? extraGR[static_cast<size_t>(i)] : 0.f;
            reductions[static_cast<size_t>(i)] = bypassed ? 0.f : alignedGR[static_cast<size_t>(next)] + amount * delayedGR[static_cast<size_t>(grNext)];
            index = next; grIndex = grNext;
        }
        if (!enabled && !mix.isSmoothing() && mix.getCurrentValue() <= 0.f && active)
        {
            active = false; oversampler->reset(); limiter.reset();
            std::fill(delayedGR.begin(), delayedGR.end(), 0.f);
        }
    }
    float reduction(int sample) const noexcept { return reductions[static_cast<size_t>(sample)]; }
private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    LookaheadLimiter limiter;
    juce::AudioBuffer<float> scratch;
    std::array<std::vector<float>, 4> aligned;
    std::vector<float> alignedGR, delayedGR;
    std::array<float, chunk> extraGR{}, reductions{};
    juce::SmoothedValue<float> mix;
    int guardDelay = 1, latency = 0, index = 0, grIndex = 0, warmup = 0, warmupLength = 1;
    float releaseCoefficient = .999f;
    bool active = false;
};
