#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <atomic>
#include <new>
#include <limits>
#include <thread>

static std::atomic<bool> watchAllocations{false};
static std::atomic<int> allocations{0};
void* operator new(std::size_t size)
{
    if (watchAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
static void param(MasterLimiterAudioProcessor& p, const char* id, float value)
{
    auto* parameter = p.getParameters().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

static void coreTests()
{
    for (double sr : {44100., 48000., 96000., 192000.})
    {
        LookaheadLimiter limiter;
        limiter.prepare(sr);
        const int delay = limiter.latency();
        // Known low-level impulse must be delayed exactly, not filtered.
        for (int i = 0; i < delay + 100; ++i)
        {
            auto frame = limiter.process(i == 0 ? .1f : 0.f, 0.f, delay / 2, .8f, .999f, 1.f);
            require(std::abs(frame.audio[0] - (i == delay ? .1f : 0.f)) < 1.e-7f, "Impulse delay incorrect");
        }
        // Independent quiet channel versus fully linked gain.
        for (float link : {0.f, 1.f})
        {
            limiter.reset();
            LookaheadLimiter::Frame frame{};
            for (int i = 0; i < delay * 3; ++i)
                frame = limiter.process(.95f, .1f, delay / 2, .5f, .999f, link);
            require(std::abs(frame.audio[0] - .5f) < 1.e-6f, "Loud channel ceiling");
            require(std::abs(frame.audio[1] - (link == 0.f ? .1f : .1f * .5f / .95f)) < 1.e-6f, "Stereo link ineffective");
        }
        // Constant signal immediately preceding a late spike: gain must react
        // before the spike, proving anticipation rather than output clipping.
        limiter.reset();
        const int spike = delay * 3, look = delay / 2;
        for (int i = 0; i <= spike + delay; ++i)
        {
            const auto f = limiter.process(i == spike ? 1.f : .1f, 0.f, look, .5f, .999f, 0.f);
            if (i == spike + delay - look)
                require(std::abs(f.audio[0] - .05f) < 1.e-6f, "No advance peak anticipation");
            if (i == spike + delay)
                require(f.audio[0] <= .500001f, "Lookahead released before spike");
        }
        // Independent analytical oracle: for zero release time, output equals
        // delayed input scaled by the actual future-window maximum. This also
        // catches incorrect tree indices that a final safety clamp could hide.
        limiter.reset();
        std::vector<float> samples(static_cast<size_t>(delay * 3));
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(.9 * std::sin(static_cast<double>(i) * .13));
        for (int i = 0; i < static_cast<int>(samples.size()); ++i)
        {
            const int anticipation = (i * 37) % (delay + 1);
            const auto f = limiter.process(samples[static_cast<size_t>(i)], 0.f, anticipation, .5f, 0.f, 0.f);
            float peak = 0.f;
            const int first = i - delay;
            for (int j = std::max(0, first); j <= first + anticipation; ++j)
                peak = std::max(peak, std::abs(samples[static_cast<size_t>(j)]));
            const float expected = first < 0 ? 0.f : samples[static_cast<size_t>(first)] * (peak > .5f ? .5f / peak : 1.f);
            require(std::abs(f.audio[0] - expected) < 2.e-6f, "Future-peak oracle mismatch");
        }
        // Hostile changing window/ceiling/link plus long ring-buffer wrap.
        std::mt19937 random(481);
        std::uniform_real_distribution<float> signal(-1.f, 1.f);
        limiter.reset();
        for (int i = 0; i < delay * 15; ++i)
        {
            const float ceiling = (i / 67) % 2 ? .5f : .9f;
            auto f = limiter.process(signal(random), signal(random), i % (delay + 1), ceiling, .999f, (i % 101) / 100.f);
            for (float sample : f.audio)
                require(std::isfinite(sample) && std::abs(sample) <= ceiling + 1.e-6f, "Dynamic ceiling exceeded");
        }
        limiter.reset();
        for (int i = 0; i < delay * 2; ++i)
        {
            const auto f = limiter.process(0.f, 0.f, delay, .7f, .99f, 1.f);
            require(f.audio[0] == 0.f && f.audio[1] == 0.f, "Reset retained audio");
        }
    }
}

static std::vector<float> render(int block, double sr, int channels, bool bypass, bool softClip = false)
{
    MasterLimiterAudioProcessor p;
    auto layout = p.getBusesLayout();
    layout.inputBuses.set(0, channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
    layout.outputBuses.set(0, layout.inputBuses[0]);
    require(p.setBusesLayout(layout), "Mono/stereo layout rejected");
    param(p, "SOFTCLIP", softClip ? 1.f : 0.f);
    param(p, "INPUT", 12.f); param(p, "LIMIT", -3.f); param(p, "LOOKAHEAD", 13.f);
    p.prepareToPlay(sr, block);
    const int delay = p.getLatencySamples();
    require(delay > static_cast<int>(std::lround(sr * .020)), "Guard latency not reported");
    std::vector<float> result;
    juce::MidiBuffer midi;
    const int total = static_cast<int>(sr / 2);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min(block, total - pos);
        juce::AudioBuffer<float> b(channels, n);
        for (int i = 0; i < n; ++i)
        {
            const float value = static_cast<float>(.7 * std::sin((pos + i) * .071));
            b.setSample(0, i, value);
            if (channels > 1) b.setSample(1, i, -value);
        }
        if (bypass) p.processBlockBypassed(b, midi); else p.processBlock(b, midi);
        for (int i = 0; i < n; ++i)
        {
            const float sample = b.getSample(0, i);
            require(std::isfinite(sample), "Nonfinite processor sample");
            if (!bypass) require(std::abs(sample) <= juce::Decibels::decibelsToGain(-3.f) + 1.e-6f, "Processor ceiling exceeded");
            if (bypass)
            {
                const float expected = pos + i >= delay ? static_cast<float>(.7 * std::sin((pos + i - delay) * .071)) : 0.f;
                require(std::abs(sample - expected) < 1.e-7f, "Bypass latency mismatch");
            }
            if (channels > 1) require(std::abs(sample + b.getSample(1, i)) < 1.e-6f, "Antiphase stereo mismatch");
            result.push_back(sample);
        }
    }
    require(std::isfinite(p.getInputRms()) && std::isfinite(p.getOutputRms()), "Invalid RMS");
    p.reset();
    juce::AudioBuffer<float> silence(channels, delay * 2);
    silence.clear();
    p.processBlock(silence, midi);
    require(silence.getMagnitude(0, silence.getNumSamples()) == 0.f, "Processor reset tail");
    return result;
}

static void stateAndAutomation()
{
    MasterLimiterAudioProcessor p, restored;
    param(p, "INPUT", 4.f); param(p, "LIMIT", -2.f); param(p, "LOOKAHEAD", 7.f);
    param(p, "RELEASE", 267.f); param(p, "LINK", .43f); param(p, "SOFTCLIP", 1.f); param(p, "TRUEPEAK", 1.f);
    juce::MemoryBlock state;
    p.getStateInformation(state);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (const char* id : {"INPUT", "LIMIT", "LOOKAHEAD", "RELEASE", "LINK", "SOFTCLIP", "TRUEPEAK"})
        require(std::abs(p.getParameters().getRawParameterValue(id)->load() - restored.getParameters().getRawParameterValue(id)->load()) < 1.e-6f, "State roundtrip mismatch");
    p.prepareToPlay(48000., 128);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> b(2, 128);
    for (int block = 0; block < 1000; ++block)
    {
        param(p, "LOOKAHEAD", block % 2 ? 0.f : 20.f);
        param(p, "LIMIT", block % 2 ? -3.f : 0.f);
        param(p, "LINK", block % 2 ? 0.f : 1.f);
        param(p, "INPUT", block % 2 ? -24.f : 24.f);
        param(p, "SOFTCLIP", block % 2 ? 0.f : 1.f);
        for (int i = 0; i < b.getNumSamples(); ++i) { b.setSample(0, i, .9f); b.setSample(1, i, .01f); }
        p.processBlock(b, midi);
        require(std::abs(p.getLatencyMs() - static_cast<float>(p.getLatencySamples()) / 48.f) < .001f, "Automation changed latency");
        require(b.getMagnitude(0, b.getNumSamples()) <= juce::Decibels::decibelsToGain(block % 2 ? -3.f : 0.f) + 1.e-6f, "Automated ceiling exceeded");
        if (block == 500) p.resetMeters();
    }
}

static void limitCompatibility()
{
    MasterLimiterAudioProcessor modern;
    require(!modern.usesLegacyLimit(), "New instance uses legacy limit");
    require(modern.getParameters().getParameter("CEILING")->convertFrom0to1(.5f) == -1.5f, "Legacy normalized automation remapped");
    require(modern.getParameters().getParameter("LIMIT")->convertFrom0to1(.5f) == -3.f, "Extended limit range");
    param(modern, "LIMIT", -6.f);
    param(modern, "CEILING", 0.f);
    require(modern.getLimitDb() == -6.f, "Dormant legacy parameter changes modern limit");
    modern.prepareToPlay(48000., 128);
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    for (int block = 0; block < 30; ++block)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i) audio.setSample(c, i, .95f);
        modern.processBlock(audio, midi);
        require(audio.getMagnitude(0, 128) <= juce::Decibels::decibelsToGain(-6.f) + 1.e-6f, "Extended -6 dB limit exceeded");
    }
    juce::MemoryBlock state;
    modern.getStateInformation(state);
    MasterLimiterAudioProcessor modernCopy;
    modernCopy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    require(!modernCopy.usesLegacyLimit() && modernCopy.getLimitDb() == -6.f, "Modern limit state lost");

    // Real previous schema: first five PARAM children, no LIMIT or mode property.
    auto oldState = modern.getParameters().copyState();
    oldState.removeProperty("legacyLimitMode", nullptr);
    for (int i = oldState.getNumChildren() - 1; i >= 0; --i)
        if (oldState.getChild(i).getProperty("id").toString() == "LIMIT" || oldState.getChild(i).getProperty("id").toString() == "SOFTCLIP") oldState.removeChild(i, nullptr);
    for (auto child : oldState)
        if (child.getProperty("id").toString() == "CEILING") child.setProperty("value", -2.f, nullptr);
    auto xml = oldState.createXml();
    juce::MemoryBlock oldBinary;
    juce::AudioProcessor::copyXmlToBinary(*xml, oldBinary);
    MasterLimiterAudioProcessor legacy;
    legacy.setStateInformation(oldBinary.getData(), static_cast<int>(oldBinary.getSize()));
    require(legacy.usesLegacyLimit() && legacy.getLimitDb() == -2.f, "Old state did not select legacy range");
    param(legacy, "CEILING", -1.5f);
    require(legacy.getLimitDb() == -1.5f, "Old ceiling automation inactive");
    param(legacy, "LIMIT", -6.f);
    require(legacy.getLimitDb() == -1.5f, "New parameter overrides old project automation");
    legacy.getStateInformation(state);
    MasterLimiterAudioProcessor legacyCopy;
    legacyCopy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    require(legacyCopy.usesLegacyLimit() && legacyCopy.getLimitDb() == -1.5f, "Legacy mode lost on resave");
    std::unique_ptr<juce::AudioProcessorEditor> editor(legacyCopy.createEditor());
    juce::Slider* limit = nullptr;
    juce::Slider* input = nullptr;
    for (int i = 0; i < editor->getNumChildComponents(); ++i)
        if (auto* slider = dynamic_cast<juce::Slider*>(editor->getChildComponent(i)))
        {
            if (slider->getName() == "LIMIT") limit = slider;
            if (slider->getName() == "INPUT") input = slider;
        }
    require(limit != nullptr && limit->getMinimum() == -3.0 && limit->getMaximum() == 0.0, "Old editor lost limit range");
    require(input != nullptr && input->valueToProportionOfLength(0.0) == .5, "Input zero not centred");
    require(input->getTextFromValue(6.0).startsWith("+") && input->getTextFromValue(-6.0).startsWith("-"), "Input signed display");
    require(limit->getX() > input->getRight(), "Limit not placed at right");
    legacyCopy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    modern.getStateInformation(state);
    legacyCopy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    juce::Thread::sleep(40);
    juce::Timer::callPendingTimersSynchronously();
    require(limit->getMinimum() == -6.0 && limit->getMaximum() == 0.0, "Open editor did not rebind after state change");
}

static void saturationTests()
{
    // A coherent 100 Hz sine below the limit must null against the delayed input.
    // The original tanh path is checked analytically as well as by its harmonics.
    for (bool clip : {false, true})
    {
        MasterLimiterAudioProcessor p;
        require(p.getParameters().getRawParameterValue("SOFTCLIP")->load() == 0.f, "New instance is not clean by default");
        param(p, "SOFTCLIP", clip ? 1.f : 0.f);
        p.prepareToPlay(48000., 128);
        juce::MidiBuffer midi;
        double fundamental = 0.0, third = 0.0;
        const int delay = p.getLatencySamples();
        constexpr int length = 4800;
        for (int pos = 0; pos < length + delay; pos += 128)
        {
            const int count = std::min(128, length + delay - pos);
            juce::AudioBuffer<float> audio(2, count);
            for (int i = 0; i < count; ++i)
            {
                const float value = static_cast<float>(.7 * std::sin(juce::MathConstants<double>::twoPi * (pos + i) / 480.));
                audio.setSample(0, i, value); audio.setSample(1, i, -value);
            }
            p.processBlock(audio, midi);
            for (int i = 0; i < count; ++i)
            {
                const int n = pos + i - delay;
                const double phase = juce::MathConstants<double>::twoPi * n / 480.;
                const float input = n < 0 ? 0.f : static_cast<float>(.7 * std::sin(phase));
                const float expected = clip ? std::tanh(input) : input;
                const float output = audio.getSample(0, i);
                require(std::abs(output - expected) < 1.e-7f, "Clean null or Soft Clip analytical reference failed");
                if (n >= 0)
                {
                    fundamental += output * std::sin(phase);
                    third += output * std::sin(3.0 * phase);
                }
            }
        }
        const double ratio = std::abs(third / fundamental);
        require(clip ? ratio > .01 : ratio < 1.e-6, "Unexpected third harmonic in saturation mode");
        std::cout << (clip ? "Soft Clip" : "Clean") << " 100 Hz, peak 0.7: H3/H1=" << ratio << '\n';
    }
    // Restore both historical schemas: before LIMIT, and after LIMIT but before
    // SOFTCLIP. These must remain tanh, including a subsequent save/load.
    for (bool oldLimit : {false, true})
    {
        MasterLimiterAudioProcessor source;
        auto tree = source.getParameters().copyState();
        for (int i = tree.getNumChildren() - 1; i >= 0; --i)
        {
            const auto id = tree.getChild(i).getProperty("id").toString();
            if (id == "SOFTCLIP" || (oldLimit && id == "LIMIT")) tree.removeChild(i, nullptr);
        }
        juce::MemoryBlock binary;
        auto xml = tree.createXml();
        juce::AudioProcessor::copyXmlToBinary(*xml, binary);
        MasterLimiterAudioProcessor restored;
        restored.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
        require(restored.getParameters().getRawParameterValue("SOFTCLIP")->load() == 1.f, "Old saturation was not migrated");
        restored.getStateInformation(binary);
        MasterLimiterAudioProcessor copy;
        copy.prepareToPlay(48000., 128);
        copy.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
        require(copy.getParameters().getRawParameterValue("SOFTCLIP")->load() == 1.f, "Old saturation lost after resave");
        juce::MidiBuffer midi;
        for (int block = 0; block < 30; ++block)
        {
            juce::AudioBuffer<float> audio(2, 128);
            for (int i = 0; i < 128; ++i) { audio.setSample(0, i, .3f); audio.setSample(1, i, .3f); }
            copy.processBlock(audio, midi);
            if (block > 10) require(std::abs(audio.getSample(0, 0) - std::tanh(.3f)) < 1.e-7f, "Legacy saturation audio changed");
        }
        param(copy, "SOFTCLIP", 0.f);
        copy.getStateInformation(binary);
        restored.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
        require(restored.getParameters().getRawParameterValue("SOFTCLIP")->load() == 0.f, "Explicit clean mode not recalled");
    }
    // Constant DC makes any switch discontinuity directly measurable, independent
    // of waveform phase. Both directions must follow the 20 ms crossfade.
    MasterLimiterAudioProcessor switched;
    switched.prepareToPlay(48000., 128);
    juce::AudioBuffer<float> dc(2, 128);
    juce::MidiBuffer midi;
    float previous = .3f;
    for (int block = 0; block < 90; ++block)
    {
        if (block == 25) param(switched, "SOFTCLIP", 1.f);
        if (block == 55) param(switched, "SOFTCLIP", 0.f);
        for (int i = 0; i < 128; ++i) { dc.setSample(0, i, .3f); dc.setSample(1, i, .3f); }
        switched.processBlock(dc, midi);
        for (int i = 0; i < 128; ++i)
        {
            const float value = dc.getSample(0, i);
            if (block >= 15) require(std::abs(value - previous) <= (.3f - std::tanh(.3f)) / 960.f + 1.e-7f, "Soft Clip switch discontinuity");
            previous = value;
        }
    }
    require(std::abs(previous - .3f) < 1.e-7f, "Crossfade did not reach clean endpoint");
    // Removing tanh must not expose NaN from a huge finite input after gain.
    param(switched, "INPUT", 24.f);
    for (int block = 0; block < 20; ++block)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i) dc.setSample(c, i, std::numeric_limits<float>::max());
        switched.processBlock(dc, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i) require(std::isfinite(dc.getSample(c, i)), "Extreme clean input is nonfinite");
    }
    require(std::isfinite(switched.getInputRms()) && std::isfinite(switched.getOutputRms()), "Extreme input poisoned meters");
}

static void realtimeAndMeters()
{
    MasterLimiterAudioProcessor p;
    p.prepareToPlay(48000., 257);
    juce::AudioBuffer<float> b(2, 257);
    juce::MidiBuffer midi;
    allocations.store(0);
    watchAllocations.store(true);
    for (int block = 0; block < 1000; ++block)
    {
        if (block == 500) p.reset();
        b.clear();
        p.processBlock(b, midi);
        p.processBlockBypassed(b, midi);
    }
    watchAllocations.store(false);
    require(allocations.load() == 0, "C++ heap allocation during audio/reset");
    p.reset();
    for (int block = 0; block < 1200; ++block)
    {
        for (int i = 0; i < 257; ++i) { b.setSample(0, i, .1f); b.setSample(1, i, .1f); }
        p.processBlock(b, midi);
    }
    require(std::abs(p.getInputRms() + 20.f) < .002f, "RMS DC calibration");
    require(std::abs(p.getOutputRms() - juce::Decibels::gainToDecibels(.1f)) < .002f, "Output RMS calibration");
    p.resetMeters();
    require(p.getPeakHold() == -100.f && p.getInputRms() == -100.f, "Stopped-transport meter reset");
    b.clear();
    b.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
    b.setSample(1, 0, std::numeric_limits<float>::infinity());
    p.processBlock(b, midi);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 257; ++i) require(std::isfinite(b.getSample(c, i)), "Nonfinite input leaked");
}

static void historyTests()
{
    SignalHistory history;
    history.prepare(1000.);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 200000; ++i)
        {
            if (i == 100000) history.reset();
            const float value = static_cast<float>((i / 2) % 100) / 100.f;
            history.push(value, -value, value * 10.f, .7f);
        }
        done.store(true);
    });
    std::array<SignalFrame, SignalHistory::capacity> frames;
    bool coherent = true;
    do
    {
        const auto count = history.copyLatest(frames);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& f = frames[i];
            coherent = coherent && f.minL == f.maxL && f.minR == -f.minL && f.maxR == -f.maxL
                && f.reduction == f.minL * 10.f && f.limit == .7f && f.sampleEnd % 2 == 0 && f.sampleRate == 1000.f;
            if (i > 0) coherent = coherent && f.sampleEnd > frames[i - 1].sampleEnd;
        }
    } while (!done.load());
    writer.join();
    require(coherent, "Mixed history frame during overwrite/reset");
    require(history.copyLatest(frames) == SignalHistory::capacity, "History did not retain newest window");
    require(history.copyLatest(frames, history.published()) == 0, "History GUI clear failed");
    for (bool tp : {false, true})
    {
        MasterLimiterAudioProcessor p;
        param(p, "TRUEPEAK", tp ? 1.f : 0.f);
        p.prepareToPlay(48000., 128);
        juce::MidiBuffer midi;
        std::vector<float> rendered;
        for (int block = 0; block < 50; ++block)
        {
            juce::AudioBuffer<float> b(2, 128);
            for (int i = 0; i < 128; ++i)
            {
                const float value = static_cast<float>(.3 * std::sin((block * 128 + i) * .13));
                b.setSample(0, i, value); b.setSample(1, i, -value);
            }
            p.processBlock(b, midi);
            for (int i = 0; i < 128; ++i) rendered.push_back(b.getSample(0, i));
        }
        const auto count = p.getSignalHistory().copyLatest(frames);
        require(count > 0, "Processor did not publish waveform");
        for (size_t i = 0; i < count; ++i)
        {
            const auto& f = frames[i];
            const size_t end = static_cast<size_t>(f.sampleEnd);
            const auto range = std::minmax_element(rendered.begin() + static_cast<std::ptrdiff_t>(end - 96), rendered.begin() + static_cast<std::ptrdiff_t>(end));
            require(f.minL == *range.first && f.maxL == *range.second && f.minR == -f.maxL && f.maxR == -f.minL, "Waveform differs from actual output or cancels antiphase");
            require(f.reduction == 0.f, "Low-level waveform has false GR");
        }
    }
}

// Independent offline reconstruction: twice as many phases and taps as the
// online meter, no JUCE oversampler or peak-guard code is reused here.
static double reconstructedPeak(const std::vector<float>& samples)
{
    constexpr int taps = 128, phases = 16;
    std::array<std::array<double, taps>, phases> kernels{};
    const double pi = juce::MathConstants<double>::pi;
    for (int phase = 0; phase < phases; ++phase)
    {
        double sum = 0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double distance = 63.0 + static_cast<double>(phase) / phases - tap;
            const double window = .42 - .5 * std::cos(2 * pi * tap / (taps - 1)) + .08 * std::cos(4 * pi * tap / (taps - 1));
            const double value = (std::abs(distance) < 1.e-12 ? 1.0 : std::sin(pi * distance) / (pi * distance)) * window;
            kernels[static_cast<size_t>(phase)][static_cast<size_t>(tap)] = value; sum += value;
        }
        for (auto& v : kernels[static_cast<size_t>(phase)]) v /= sum;
    }
    std::vector<float> padded(samples.size() + taps * 2, 0.f);
    std::copy(samples.begin(), samples.end(), padded.begin() + taps);
    double peak = 0;
    for (size_t n = 0; n + taps < padded.size(); ++n)
        for (const auto& kernel : kernels)
        {
            double value = 0;
            for (size_t tap = 0; tap < taps; ++tap) value += padded[n + tap] * kernel[tap];
            peak = std::max(peak, std::abs(value));
        }
    return peak;
}

static std::vector<float> renderPeakCase(double sr, int pattern, bool tp, int block, bool clip = false, float limitDb = -1.f)
{
    MasterLimiterAudioProcessor p;
    param(p, "INPUT", 12.f); param(p, "LIMIT", limitDb); param(p, "TRUEPEAK", tp ? 1.f : 0.f); param(p, "SOFTCLIP", clip ? 1.f : 0.f);
    p.prepareToPlay(sr, block);
    const int active = static_cast<int>(sr * .060), total = active + p.getLatencySamples() + static_cast<int>(sr * .010);
    std::vector<float> result;
    juce::MidiBuffer midi;
    std::mt19937 random(12345);
    std::uniform_real_distribution<float> noise(-.8f, .8f);
    const double pi = juce::MathConstants<double>::pi;
    for (int pos = 0; pos < total; pos += block)
    {
        const int count = std::min(block, total - pos);
        juce::AudioBuffer<float> b(2, count);
        for (int i = 0; i < count; ++i)
        {
            const int n = pos + i;
            float value = 0;
            if (n < active)
            {
                if (pattern == 0) value = static_cast<float>(.8 * std::sin(2 * pi * .25 * n + pi / 4));
                if (pattern == 1) value = static_cast<float>(.8 * std::sin(2 * pi * .45 * n + .21));
                if (pattern == 2) value = noise(random);
                if (pattern == 3) value = n % 997 == 0 ? (n % 2 ? -.9f : .9f) : 0.f;
                if (pattern == 4) value = static_cast<float>(.5 * std::sin(2 * pi * 70.0 * n / sr) + .4 * std::exp(-(n % static_cast<int>(sr * .025)) / (sr * .004)) * std::cos(2 * pi * 650.0 * n / sr));
                if (pattern == 5) value = (n / 23) % 2 ? -.8f : .8f;
                if (pattern == 6) value = static_cast<float>(.8 * std::sin(2 * pi * .49 * n + .31));
            }
            b.setSample(0, i, value); b.setSample(1, i, -value);
        }
        p.processBlock(b, midi);
        for (int i = 0; i < count; ++i)
        {
            const float value = b.getSample(0, i);
            require(std::isfinite(value) && std::abs(value + b.getSample(1, i)) < 2.e-6f, "TP stereo/nonfinite output");
            result.push_back(value);
        }
    }
    return result;
}

static void truePeakTests()
{
    const double ceiling = juce::Decibels::decibelsToGain(-1.0);
    const double spPeak = reconstructedPeak(renderPeakCase(48000., 0, false, 257));
    require(spPeak > ceiling * 1.1, "Intersample fixture does not exceed sample ceiling");
    double worst = -100.;
    bool allSafe = true;
    for (float limit : {-6.f, -1.f, 0.f})
    for (double sr : {44100., 48000., 96000., 192000.})
        for (int pattern = 0; pattern < 7; ++pattern)
        {
            const double ceiling = juce::Decibels::decibelsToGain(static_cast<double>(limit));
            const auto output = renderPeakCase(sr, pattern, true, 257, false, limit);
            const double peak = reconstructedPeak(output);
            const double db = juce::Decibels::gainToDecibels(peak);
            worst = std::max(worst, db - limit);
            if (peak > ceiling * std::pow(10.0, .1 / 20.0))
                std::cerr << "TP overshoot limit=" << limit << " sr=" << sr << " pattern=" << pattern << " peak=" << db << " dBTP\n";
            allSafe = allSafe && peak <= ceiling * std::pow(10.0, .1 / 20.0);
        }
    require(allSafe, "16x oracle exceeded true-peak limit by >0.1 dB");
    const auto reference = renderPeakCase(48000., 4, true, 1);
    const auto other = renderPeakCase(48000., 4, true, 1024);
    require(reference.size() == other.size(), "TP render length differs");
    for (size_t i = 0; i < reference.size(); ++i) require(std::abs(reference[i] - other[i]) < 2.e-6f, "TP depends on block size");
    const auto clipped = renderPeakCase(48000., 5, true, 64, true);
    require(reconstructedPeak(clipped) <= ceiling * std::pow(10.0, .1 / 20.0), "Soft Clip + TP exceeded limit");
    MasterLimiterAudioProcessor p;
    param(p, "TRUEPEAK", 1.f); p.prepareToPlay(48000., 128);
    const int latency = p.getLatencySamples();
    juce::AudioBuffer<float> b(2, 128); juce::MidiBuffer midi;
    allocations.store(0); watchAllocations.store(true);
    for (int n = 0; n < 100; ++n)
    {
        b.clear(); p.processBlock(b, midi); p.processBlockBypassed(b, midi);
        if (n == 50) p.reset();
    }
    watchAllocations.store(false);
    require(allocations.load() == 0, "TP process/reset allocated C++ heap");
    param(p, "TRUEPEAK", 0.f);
    allocations.store(0); watchAllocations.store(true);
    for (int n = 0; n < 50; ++n) { b.clear(); p.processBlock(b, midi); }
    watchAllocations.store(false);
    require(allocations.load() == 0, "TP deactivation allocated C++ heap");
    param(p, "TRUEPEAK", 1.f);
    allocations.store(0); watchAllocations.store(true);
    for (int n = 0; n < 50; ++n) { b.clear(); p.processBlock(b, midi); }
    watchAllocations.store(false);
    require(allocations.load() == 0, "TP activation allocated C++ heap");
    require(p.getLatencySamples() == latency, "TP switching changed host latency");
    TruePeakMeter meter;
    float knownPeak = 0.f;
    for (int i = 0; i < 1000; ++i)
    {
        const float sample = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * .25 * i + juce::MathConstants<double>::pi / 4.));
        const float peak = meter.process(sample, -sample);
        if (i > 128) knownPeak = juce::jmax(knownPeak, peak);
    }
    require(std::abs(juce::Decibels::gainToDecibels(knownPeak)) < .02f, "TP meter intersample calibration");
    meter.reset();
    require(meter.process(0.f, 0.f) == 0.f, "TP meter retained history after reset");
    MasterLimiterAudioProcessor switching;
    switching.prepareToPlay(48000., 128);
    const int stableDelay = switching.getLatencySamples();
    for (int n = 0; n < 600; ++n)
    {
        if (n == 100 || n == 450) param(switching, "TRUEPEAK", 1.f);
        if (n == 300) param(switching, "TRUEPEAK", 0.f);
        for (int i = 0; i < 128; ++i)
        {
            const float value = static_cast<float>(.1 * std::sin(juce::MathConstants<double>::twoPi * 997.0 * (n * 128 + i) / 48000.));
            b.setSample(0, i, value); b.setSample(1, i, value);
        }
        switching.processBlock(b, midi);
        for (int i = 0; i < 128; ++i)
        {
            const int t = n * 128 + i - stableDelay;
            const float expected = t < 0 ? 0.f : static_cast<float>(.1 * std::sin(juce::MathConstants<double>::twoPi * 997.0 * t / 48000.));
            require(std::abs(b.getSample(0, i) - expected) < .0002f, "TP switching misaligned the waveform");
        }
        require(switching.getLatencySamples() == stableDelay, "TP mode changed latency");
    }
    // Old state loaded into a previously TP-enabled instance must turn it off.
    auto state = p.getParameters().copyState();
    for (int i = state.getNumChildren() - 1; i >= 0; --i)
        if (state.getChild(i).getProperty("id").toString() == "TRUEPEAK") state.removeChild(i, nullptr);
    auto xml = state.createXml(); juce::MemoryBlock binary;
    juce::AudioProcessor::copyXmlToBinary(*xml, binary);
    p.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
    require(!p.isTruePeakEnabled(), "Old state kept unrelated TP mode");
    std::cout << "TP: SP fixture=" << juce::Decibels::gainToDecibels(spPeak) << " dBTP; worst protected deviation=" << worst << " dB relative to Limit; latency=" << p.getLatencyMs() << " ms\n";
}

static void previews()
{
    MasterLimiterAudioProcessor p;
    param(p, "INPUT", 9.f); param(p, "LIMIT", -3.f); param(p, "TRUEPEAK", 1.f);
    p.prepareToPlay(48000., 1600);
    std::array<float, 8> before{};
    const std::array<const char*, 8> ids{"INPUT", "CEILING", "LOOKAHEAD", "RELEASE", "LINK", "LIMIT", "SOFTCLIP", "TRUEPEAK"};
    for (size_t i = 0; i < ids.size(); ++i) before[i] = p.getParameters().getRawParameterValue(ids[i])->load();
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    juce::AudioBuffer<float> audio(2, 1600);
    juce::MidiBuffer midi;
    // Feed real processor readings through the editor's normal timer.
    for (int frame = 0; frame < 180; ++frame)
    {
        const float amplitude = .02f + .98f * static_cast<float>(.5 + .5 * std::sin(frame * .09));
        for (int i = 0; i < 1600; ++i)
        {
            const float sample = amplitude * static_cast<float>(std::sin((frame * 1600 + i) * .071));
            audio.setSample(0, i, sample); audio.setSample(1, i, sample * .8f);
        }
        p.processBlock(audio, midi);
        juce::Thread::sleep(35);
        juce::Timer::callPendingTimersSynchronously();
    }
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile("previews");
    dir.createDirectory();
    for (auto size : {juce::Point<int>(900, 580), {1100, 700}, {1400, 900}, {1800, 1100}})
    {
        editor->setSize(size.x, size.y);
        for (int child = 0; child < editor->getNumChildComponents(); ++child)
        {
            const auto bounds = editor->getChildComponent(child)->getBounds();
            require(bounds.getWidth() > 0 && bounds.getHeight() > 0 && editor->getLocalBounds().contains(bounds), "Editor child outside window");
        }
        juce::Image image(juce::Image::ARGB, size.x, size.y, true);
        juce::Graphics g(image);
        editor->paintEntireComponent(g, false);
        auto stream = dir.getChildFile("limiter-" + juce::String(size.x) + "x" + juce::String(size.y) + ".png").createOutputStream();
        require(stream != nullptr, "Preview stream failed");
        stream->setPosition(0);
        stream->truncate();
        require(juce::PNGImageFormat().writeImageToStream(image, *stream), "Preview export failed");
    }
    for (size_t i = 0; i < ids.size(); ++i)
        require(p.getParameters().getRawParameterValue(ids[i])->load() == before[i], "Editor changed parameters");
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI init;
        coreTests();
        for (double sr : {44100., 48000., 96000., 192000.})
            for (int channels : {1, 2})
            for (bool softClip : {false, true})
            {
                const auto reference = render(1, sr, channels, false, softClip);
                for (int block : {64, 257, 1024})
                {
                    const auto output = render(block, sr, channels, false, softClip);
                    require(output == reference, "Rendering depends on host block size");
                }
                render(257, sr, channels, true, softClip);
            }
        stateAndAutomation();
        limitCompatibility();
        realtimeAndMeters();
        saturationTests();
        historyTests();
        truePeakTests();
        if (std::getenv("MASTERLIMITER_PREVIEWS") != nullptr) previews();
        std::cout << "PASS: delay, anticipation, ceiling, stereo link, mono/stereo, antiphase, block invariance, bypass, reset, state, automation\n";
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
