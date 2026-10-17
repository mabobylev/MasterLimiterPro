#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include <array>

class InputLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
};

class HistoryComponent : public juce::Component
{
public:
    explicit HistoryComponent(const SignalHistory& h) : source(h) {}
    void update();
    void clear();
    void paint(juce::Graphics&) override;
private:
    const SignalHistory& source;
    std::array<SignalFrame, SignalHistory::capacity> frames{};
    size_t size = 0;
    uint64_t clearBefore = 0;
};

class MasterLimiterAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MasterLimiterAudioProcessorEditor(MasterLimiterAudioProcessor&);
    ~MasterLimiterAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    MasterLimiterAudioProcessor& processor;
    HistoryComponent history;
    InputLookAndFeel inputLookAndFeel;
    juce::Slider input, ceiling, lookahead, release, link;
    bool legacyLimitBinding = false;
    void bindLimit();
    juce::Label inputL, ceilingL, lookaheadL, releaseL, linkL;
    juce::TextButton resetButton{"RESET METERS"}, softClipButton{"SOFT CLIP"}, truePeakButton{"TRUE PEAK"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> softClipAttachment, truePeakAttachment;
    juce::Rectangle<int> graphPanel, metersPanel, controlsPanel;
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::array<std::unique_ptr<Attachment>, 5> attachments;
    float inputDb = -100.f, outputDb = -100.f, reductionDb = 0.f;
    float inputRms = -100.f, outputRms = -100.f, outputHold = -100.f;
    void timerCallback() override;
    void setupSlider(juce::Slider&, juce::Label&, const juce::String&, juce::Colour, double);
    void drawMeter(juce::Graphics&, juce::Rectangle<float>, float, const juce::String&, juce::Colour, bool);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterLimiterAudioProcessorEditor)
};
