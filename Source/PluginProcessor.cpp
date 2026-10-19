#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <limits>

static float safeDb(float x) { return juce::Decibels::gainToDecibels(juce::jmax(x, 1.0e-7f), -100.0f); }

MasterLimiterAudioProcessor::MasterLimiterAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
    inputParameter = parameters.getRawParameterValue("INPUT");
    ceilingParameter = parameters.getRawParameterValue("CEILING");
    limitParameter = parameters.getRawParameterValue("LIMIT");
    softClipParameter = parameters.getRawParameterValue("SOFTCLIP");
    truePeakParameter = parameters.getRawParameterValue("TRUEPEAK");
    lookaheadParameter = parameters.getRawParameterValue("LOOKAHEAD");
    releaseParameter = parameters.getRawParameterValue("RELEASE");
    linkParameter = parameters.getRawParameterValue("LINK");
}

juce::AudioProcessorValueTreeState::ParameterLayout
MasterLimiterAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "INPUT", "Input", juce::NormalisableRange<float>(-24.f, 24.f, .01f), 0.f, "dB"));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "CEILING", "Ceiling (legacy)", juce::NormalisableRange<float>(-3.f, 0.f, .01f), -1.f, "dB"));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "LOOKAHEAD", "Lookahead", juce::NormalisableRange<float>(0.f, 20.f, .01f), 5.f, "ms"));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "RELEASE", "Release", juce::NormalisableRange<float>(10.f, 1000.f, 1.f, .4f), 100.f, "ms"));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "LINK", "Stereo Link", juce::NormalisableRange<float>(0.f, 1.f, .01f), 1.f));
    // Append the extended limit; do not remap old normalized CEILING automation.
    p.push_back(std::make_unique<juce::AudioParameterFloat>(
        "LIMIT", "Limit", juce::NormalisableRange<float>(-6.f, 0.f, .01f), -1.f, "dB"));
    p.push_back(std::make_unique<juce::AudioParameterBool>("SOFTCLIP", "Soft Clip", false));
    p.push_back(std::make_unique<juce::AudioParameterBool>("TRUEPEAK", "True Peak", false));
    return { p.begin(), p.end() };
}

void MasterLimiterAudioProcessor::prepareToPlay(double sr, int)
{
    currentSampleRate = sr;
    limiter.prepare(sr);
    truePeakGuard.prepare(sr);
    truePeakGuard.start(isTruePeakEnabled(), sr);
    truePeakMeter.reset();
    signalHistory.prepare(sr);
    loudnessMeter.prepare(sr);
    setLatencySamples(limiter.latency() + truePeakGuard.getLatency());
    latencyMs.store(1000.f * static_cast<float>(getLatencySamples() / sr));
    for (auto& channel : dryDelay)
        channel.assign(static_cast<size_t>(limiter.latency() + 1), 0.f);
    dryIndex = 0;
    hasProcessedAudio = false;
    const std::array<juce::SmoothedValue<float>*, 6> smoothed{
        &inputSmoothing, &ceilingSmoothing, &lookaheadSmoothing, &releaseSmoothing, &linkSmoothing, &softClipSmoothing};
    const std::array<float, 6> values{inputParameter->load(), getLimitDb(),
        lookaheadParameter->load(), releaseParameter->load(), linkParameter->load(), softClipParameter->load()};
    for (size_t i = 0; i < smoothed.size(); ++i)
    {
        smoothed[i]->reset(sr, .020);
        smoothed[i]->setCurrentAndTargetValue(values[i]);
    }
    clearMeterState();
    prepared = true;
}

void MasterLimiterAudioProcessor::reset()
{
    limiter.reset();
    truePeakGuard.reset();
    truePeakGuard.start(isTruePeakEnabled(), currentSampleRate);
    truePeakMeter.reset();
    for (auto& channel : dryDelay) std::fill(channel.begin(), channel.end(), 0.f);
    dryIndex = 0;
    hasProcessedAudio = false;
    inputSmoothing.setCurrentAndTargetValue(inputParameter->load());
    ceilingSmoothing.setCurrentAndTargetValue(getLimitDb());
    lookaheadSmoothing.setCurrentAndTargetValue(lookaheadParameter->load());
    releaseSmoothing.setCurrentAndTargetValue(releaseParameter->load());
    linkSmoothing.setCurrentAndTargetValue(linkParameter->load());
    softClipSmoothing.setCurrentAndTargetValue(softClipParameter->load());
    clearMeterState();
}

void MasterLimiterAudioProcessor::resetMeters()
{
    meterResetRequested.store(true);
    // Clear the published readouts even when transport/audio processing is stopped.
    gainReductionDb.store(0.f);
    inputPeakDb.store(-100.f); outputPeakDb.store(-100.f);
    inputRms.store(-100.f); outputRms.store(-100.f);
    peakHoldDb.store(-100.f);
    outputTruePeakDb.store(-100.f);
}

void MasterLimiterAudioProcessor::clearMeterState()
{
    inputPower = outputPower = 0.0;
    loudnessMeter.reset();
    publishLoudness({-100.f, -100.f, -100.f});
    displayTruePeak = -100.f;
    signalHistory.reset();
    displayInput = displayOutput = heldOutput = -100.f;
    displayGR = 0.f;
    holdSamples = 0;
    gainReductionDb.store(0.f);
    inputPeakDb.store(-100.f); outputPeakDb.store(-100.f);
    inputRms.store(-100.f); outputRms.store(-100.f);
    peakHoldDb.store(-100.f);
    outputTruePeakDb.store(-100.f);
}

void MasterLimiterAudioProcessor::releaseResources() { prepared = false; }

bool MasterLimiterAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getChannelSet(true, 0);
    const auto out = l.getChannelSet(false, 0);
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return in == out;
}

void MasterLimiterAudioProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
{
    process(b, false);
}

void MasterLimiterAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
{
    process(b, true);
}

void MasterLimiterAudioProcessor::process(juce::AudioBuffer<float>& b, bool bypassed)
{
    juce::ScopedNoDenormals nd;
    const int ch = juce::jmin(2, b.getNumChannels());
    if (!prepared || ch == 0) { b.clear(); return; }
    if (meterResetRequested.exchange(false)) clearMeterState();
    // Hosts may restore state after prepareToPlay. Start the first real block
    // at those restored values, then smooth subsequent live automation.
    if (!hasProcessedAudio && b.getNumSamples() > 0)
    {
        inputSmoothing.setCurrentAndTargetValue(inputParameter->load());
        ceilingSmoothing.setCurrentAndTargetValue(getLimitDb());
        lookaheadSmoothing.setCurrentAndTargetValue(lookaheadParameter->load());
        releaseSmoothing.setCurrentAndTargetValue(releaseParameter->load());
        linkSmoothing.setCurrentAndTargetValue(linkParameter->load());
        softClipSmoothing.setCurrentAndTargetValue(softClipParameter->load());
        truePeakGuard.start(isTruePeakEnabled(), currentSampleRate);
        hasProcessedAudio = true;
    }
    inputSmoothing.setTargetValue(inputParameter->load());
    const float ceilingTarget = getLimitDb();
    ceilingSmoothing.setTargetValue(ceilingTarget);
    lookaheadSmoothing.setTargetValue(lookaheadParameter->load());
    releaseSmoothing.setTargetValue(releaseParameter->load());
    linkSmoothing.setTargetValue(linkParameter->load());
    softClipSmoothing.setTargetValue(softClipParameter->load());
    const double rmsCoefficient = std::exp(-1.0 / (currentSampleRate * .300));
    const float meterDecay = static_cast<float>(24.0 / currentSampleRate);
    const float holdDecay = static_cast<float>(20.0 / currentSampleRate);
    for (int offset = 0; offset < b.getNumSamples(); offset += TruePeakGuard::chunk)
    {
        const int count = juce::jmin(TruePeakGuard::chunk, b.getNumSamples() - offset);
        for (int i = 0; i < count; ++i)
        {
            float rawL = b.getSample(0, offset + i), rawR = ch > 1 ? b.getSample(1, offset + i) : rawL;
            if (!std::isfinite(rawL)) rawL = 0.f;
            if (!std::isfinite(rawR)) rawR = 0.f;
            const float inputGain = juce::Decibels::decibelsToGain(inputSmoothing.getNextValue());
            // A falling ceiling takes effect immediately for sample-peak safety;
            // upward moves are ramped, as are the remaining controls.
            const float ceiling = juce::Decibels::decibelsToGain(juce::jmin(ceilingSmoothing.getNextValue(), ceilingTarget));
            const int anticipation = juce::roundToInt(static_cast<float>(currentSampleRate * .001) * lookaheadSmoothing.getNextValue());
            const float releaseCoefficient = std::exp(-1.f / static_cast<float>(currentSampleRate * .001 * releaseSmoothing.getNextValue()));
            const float link = linkSmoothing.getNextValue();
            const double maxFloat = static_cast<double>(std::numeric_limits<float>::max());
            const float inL = static_cast<float>(std::clamp(static_cast<double>(rawL) * inputGain, -maxFloat, maxFloat));
            const float inR = static_cast<float>(std::clamp(static_cast<double>(rawR) * inputGain, -maxFloat, maxFloat));
            const float clipMix = softClipSmoothing.getNextValue();
            const auto saturate = [clipMix](float sample) noexcept {
                if (clipMix <= 0.f) return sample;
                const float soft = std::tanh(sample);
                if (clipMix >= 1.f) return soft;
                return sample * (1.f - clipMix) + soft * clipMix;
            };
            const auto limited = limiter.process(saturate(inL), saturate(inR), anticipation, ceiling, releaseCoefficient, link);
            const int dryRead = (dryIndex + 1) % static_cast<int>(dryDelay[0].size());
            dryDelay[0][static_cast<size_t>(dryIndex)] = rawL;
            dryDelay[1][static_cast<size_t>(dryIndex)] = rawR;
            workL[static_cast<size_t>(i)] = limited.audio[0]; workR[static_cast<size_t>(i)] = limited.audio[1];
            workDryL[static_cast<size_t>(i)] = dryDelay[0][static_cast<size_t>(dryRead)];
            workDryR[static_cast<size_t>(i)] = dryDelay[1][static_cast<size_t>(dryRead)];
            workCeiling[static_cast<size_t>(i)] = ceiling; workLink[static_cast<size_t>(i)] = link;
            workGR[static_cast<size_t>(i)] = limited.reduction;
            dryIndex = dryRead;
            const double inEnergy = (static_cast<double>(inL) * inL + static_cast<double>(inR) * inR) * .5;
            inputPower = rmsCoefficient * inputPower + (1.0 - rmsCoefficient) * inEnergy;
            const float ip = safeDb(juce::jmax(std::abs(inL), std::abs(inR)));
            displayInput = juce::jmax(ip, displayInput - meterDecay);
        }
        truePeakGuard.process(workL.data(), workR.data(), workDryL.data(), workDryR.data(), workCeiling.data(), workLink.data(), workGR.data(), count, isTruePeakEnabled(), bypassed);
        for (int i = 0; i < count; ++i)
        {
            const float outL = workL[static_cast<size_t>(i)], outR = workR[static_cast<size_t>(i)];
            b.setSample(0, offset + i, outL);
            if (ch > 1) b.setSample(1, offset + i, outR);
            const double outEnergy = (static_cast<double>(outL) * outL + static_cast<double>(outR) * outR) * .5;
            outputPower = rmsCoefficient * outputPower + (1.0 - rmsCoefficient) * outEnergy;
            const float op = safeDb(juce::jmax(std::abs(outL), std::abs(outR)));
            displayOutput = juce::jmax(op, displayOutput - meterDecay);
            const float gr = truePeakGuard.reduction(i);
            displayGR = juce::jmax(gr, displayGR - meterDecay);
            displayTruePeak = juce::jmax(safeDb(truePeakMeter.process(outL, outR)), displayTruePeak - meterDecay);
            signalHistory.push(outL, outR, gr, workCeiling[static_cast<size_t>(i)]);
            loudnessMeter.process(outL, outR, ch);
            if (op >= heldOutput) { heldOutput = op; holdSamples = juce::roundToInt(currentSampleRate); }
            else if (holdSamples > 0) --holdSamples;
            else heldOutput = juce::jmax(op, heldOutput - holdDecay);
        }
    }
    inputPeakDb.store(displayInput); outputPeakDb.store(displayOutput);
    inputRms.store(safeDb(static_cast<float>(std::sqrt(inputPower))));
    outputRms.store(safeDb(static_cast<float>(std::sqrt(outputPower))));
    peakHoldDb.store(heldOutput); gainReductionDb.store(displayGR);
    outputTruePeakDb.store(displayTruePeak);
    publishLoudness(loudnessMeter.readings());
}

void MasterLimiterAudioProcessor::publishLoudness(LoudnessMeter::Readings readings) noexcept
{
    // One audio owner, SC atomic payload and sequence: GUI never accepts a
    // mixture of different M/S/I publications. GUI reset only requests a clear.
    loudnessSequence.fetch_add(1);
    loudnessM.store(readings.momentary);
    loudnessS.store(readings.shortTerm);
    loudnessI.store(readings.integrated);
    loudnessSequence.fetch_add(1);
}

void MasterLimiterAudioProcessor::getStateInformation(juce::MemoryBlock& d)
{
    auto state = parameters.copyState();
    state.setProperty("legacyLimitMode", usesLegacyLimit(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, d);
}

void MasterLimiterAudioProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
    {
        if (!xml->hasTagName(parameters.state.getType())) return;
        auto state = juce::ValueTree::fromXml(*xml);
        bool hasExtendedLimit = false;
        bool hasSoftClip = false;
        bool hasTruePeak = false;
        for (const auto& child : state)
        {
            const auto id = child.getProperty("id").toString();
            if (id == "LIMIT") hasExtendedLimit = true;
            if (id == "SOFTCLIP") hasSoftClip = true;
            if (id == "TRUEPEAK") hasTruePeak = true;
        }
        // States predating this control always processed through tanh.
        // Store the explicit value so subsequent saves retain the same sound.
        if (!hasSoftClip)
        {
            juce::ValueTree clipState("PARAM");
            clipState.setProperty("id", "SOFTCLIP", nullptr);
            clipState.setProperty("value", 1.f, nullptr);
            state.appendChild(clipState, nullptr);
        }
        if (!hasTruePeak)
        {
            juce::ValueTree tpState("PARAM");
            tpState.setProperty("id", "TRUEPEAK", nullptr);
            tpState.setProperty("value", 0.f, nullptr);
            state.appendChild(tpState, nullptr);
        }
        const bool legacy = !hasExtendedLimit || static_cast<bool>(state.getProperty("legacyLimitMode", false));
        parameters.replaceState(state);
        legacyLimitMode.store(legacy);
    }
}

juce::AudioProcessorEditor* MasterLimiterAudioProcessor::createEditor()
{
    return new MasterLimiterAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MasterLimiterAudioProcessor();
}
