#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "LookaheadLimiter.h"
#include "SignalHistory.h"
#include "TruePeakGuard.h"
#include "TruePeakMeter.h"
#include "LoudnessMeter.h"

static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free
              && std::atomic<unsigned>::is_always_lock_free,
              "Audio meter atomics must be lock-free");

class MasterLimiterAudioProcessor : public juce::AudioProcessor
{
public:
    MasterLimiterAudioProcessor();
    ~MasterLimiterAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Master Limiter Pro"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return .050; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    float getGainReduction() const { return gainReductionDb.load(); }
    float getInputPeak() const { return inputPeakDb.load(); }
    float getOutputPeak() const { return outputPeakDb.load(); }
    float getInputRms() const { return inputRms.load(); }
    float getOutputRms() const { return outputRms.load(); }
    float getPeakHold() const { return peakHoldDb.load(); }
    float getOutputTruePeak() const { return outputTruePeakDb.load(); }
    float getLatencyMs() const { return latencyMs.load(); }
    const SignalHistory& getSignalHistory() const { return signalHistory; }
    bool isTruePeakEnabled() const { return truePeakParameter->load() >= .5f; }
    LoudnessMeter::Readings getOutputLoudness() const
    {
        if (meterResetRequested.load()) return {-100.f, -100.f, -100.f};
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = loudnessSequence.load();
            if ((before & 1u) != 0) continue;
            const LoudnessMeter::Readings result{loudnessM.load(), loudnessS.load(), loudnessI.load()};
            if (before == loudnessSequence.load()) return result;
        }
        return {-100.f, -100.f, -100.f};
    }
    void resetMeters();
    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }

    bool usesLegacyLimit() const { return legacyLimitMode.load(); }
    const char* getLimitParameterID() const { return usesLegacyLimit() ? "CEILING" : "LIMIT"; }
    float getLimitDb() const { return usesLegacyLimit() ? ceilingParameter->load() : limitParameter->load(); }
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* inputParameter = nullptr;
    std::atomic<float>* ceilingParameter = nullptr;
    std::atomic<float>* limitParameter = nullptr;
    std::atomic<float>* softClipParameter = nullptr;
    std::atomic<float>* truePeakParameter = nullptr;
    std::atomic<bool> legacyLimitMode{false};
    std::atomic<float>* lookaheadParameter = nullptr;
    std::atomic<float>* releaseParameter = nullptr;
    std::atomic<float>* linkParameter = nullptr;
    LookaheadLimiter limiter;
    TruePeakGuard truePeakGuard;
    TruePeakMeter truePeakMeter;
    LoudnessMeter loudnessMeter;
    std::atomic<unsigned> loudnessSequence{0};
    void publishLoudness(LoudnessMeter::Readings) noexcept;
    std::atomic<float> loudnessM{-100.f}, loudnessS{-100.f}, loudnessI{-100.f};
    SignalHistory signalHistory;
    std::array<float, TruePeakGuard::chunk> workL{}, workR{}, workDryL{}, workDryR{}, workCeiling{}, workLink{}, workGR{};
    float displayTruePeak = -100.f;
    std::atomic<float> outputTruePeakDb{-100.f}, latencyMs{20.f};
    juce::SmoothedValue<float> inputSmoothing, ceilingSmoothing, lookaheadSmoothing, releaseSmoothing, linkSmoothing, softClipSmoothing;
    std::array<std::vector<float>, 2> dryDelay;
    int dryIndex = 0;
    bool prepared = false;
    bool hasProcessedAudio = false;
    std::atomic<bool> meterResetRequested{false};
    double inputPower = 0.0, outputPower = 0.0;
    float heldOutput = -100.f, displayInput = -100.f, displayOutput = -100.f, displayGR = 0.f;
    int holdSamples = 0;
    void process(juce::AudioBuffer<float>&, bool bypassed);
    void clearMeterState();
    double currentSampleRate = 44100.0;
    std::atomic<float> gainReductionDb { 0.0f };
    std::atomic<float> inputPeakDb { -100.0f };
    std::atomic<float> outputPeakDb { -100.0f };
    std::atomic<float> inputRms { -100.0f };
    std::atomic<float> outputRms { -100.0f };
    std::atomic<float> peakHoldDb { -100.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterLimiterAudioProcessor)
};
