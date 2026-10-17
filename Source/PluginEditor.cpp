#include "PluginEditor.h"
#include <cmath>

namespace
{
const juce::Colour bg(7, 9, 13), panel(16, 19, 25), panel2(11, 14, 19);
const juce::Colour grid(38, 43, 52), gridBright(61, 68, 80);
const juce::Colour text(236, 240, 247), muted(133, 141, 154);
const juce::Colour orange(255, 158, 68), green(69, 222, 153);
const juce::Colour purple(191, 118, 255), pink(255, 91, 116);
juce::Font font(float height, bool bold = false)
{
    return juce::Font(juce::FontOptions{}.withHeight(height).withStyle(bold ? "Bold" : "Regular"));
}
juce::String dbText(float db)
{
    return db <= -99.f ? "-inf dB" : juce::String(db, 1) + " dB";
}
void box(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour(panel2);
    g.fillRoundedRectangle(bounds.toFloat(), 12.f);
    g.setColour(grid);
    g.drawRoundedRectangle(bounds.toFloat().reduced(.5f), 12.f, 1.f);
}
}

void InputLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
    float position, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(width), static_cast<float>(height)).reduced(8.f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * .5f - 4.f;
    const auto centre = bounds.getCentre();
    const float zero = startAngle + static_cast<float>(slider.valueToProportionOfLength(0.0)) * (endAngle - startAngle);
    const float angle = startAngle + position * (endAngle - startAngle);
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius, radius, 0.f, startAngle, endAngle, true);
    g.setColour(gridBright);
    g.strokePath(track, juce::PathStrokeType(7.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (std::abs(angle - zero) > .001f)
    {
        juce::Path active;
        active.addCentredArc(centre.x, centre.y, radius, radius, 0.f, juce::jmin(zero, angle), juce::jmax(zero, angle), true);
        g.setColour(orange);
        g.strokePath(active, juce::PathStrokeType(7.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const auto point = [centre](float a, float r) { return centre + juce::Point<float>(std::sin(a) * r, -std::cos(a) * r); };
    g.setColour(text);
    g.drawLine({point(zero, radius - 8.f), point(zero, radius + 8.f)}, 1.5f);
    g.setFont(font(16.f, true));
    g.setColour(text.withAlpha(.85f));
    const float signY = centre.y + radius * .70f - 10.f;
    g.drawText("-", centre.x - radius - 23.f, signY, 20.f, 20.f, juce::Justification::centred);
    g.drawText("+", centre.x + radius + 3.f, signY, 20.f, 20.f, juce::Justification::centred);
    const auto pointer = point(angle, radius);
    g.setColour(orange);
    g.fillEllipse(pointer.x - 6.f, pointer.y - 6.f, 12.f, 12.f);
}

void HistoryComponent::clear()
{
    clearBefore = source.published(); size = 0; repaint();
}

void HistoryComponent::update()
{
    size = source.copyLatest(frames, clearBefore);
    repaint();
}

void HistoryComponent::paint(juce::Graphics& g)
{
    auto plot = getLocalBounds().toFloat().reduced(4.f, 6.f);
    plot.removeFromRight(34.f);
    auto grArea = plot.removeFromTop(juce::jmax(40.f, plot.getHeight() * .25f));
    plot.removeFromTop(14.f);
    const auto leftArea = plot.removeFromTop((plot.getHeight() - 12.f) * .5f);
    plot.removeFromTop(12.f);
    const auto rightArea = plot;
    g.setFont(font(9.f));
    for (int i = 0; i <= 6; ++i)
    {
        g.setColour(grid.withAlpha(.65f));
        const int x = juce::roundToInt(grArea.getX() + grArea.getWidth() * static_cast<float>(i) / 6.f);
        g.drawVerticalLine(x, grArea.getY(), rightArea.getBottom());
    }
    for (int db : {0, 12, 24})
    {
        const float y = grArea.getY() + grArea.getHeight() * static_cast<float>(db) / 24.f;
        g.setColour(grid); g.drawHorizontalLine(juce::roundToInt(y), grArea.getX(), grArea.getRight());
        g.setColour(muted); g.drawText(juce::String(-db), grArea.getRight() + 6.f, y - 6.f, 28.f, 12.f, juce::Justification::left);
    }
    for (const auto area : {leftArea, rightArea})
    {
        g.setColour(gridBright.withAlpha(.7f));
        g.drawHorizontalLine(juce::roundToInt(area.getCentreY()), area.getX(), area.getRight());
        g.setColour(grid.withAlpha(.5f));
        g.drawHorizontalLine(juce::roundToInt(area.getY()), area.getX(), area.getRight());
        g.drawHorizontalLine(juce::roundToInt(area.getBottom()), area.getX(), area.getRight());
        g.setColour(muted);
        g.drawText("+1", area.getRight() + 6.f, area.getY() - 4.f, 28.f, 12.f, juce::Justification::left);
        g.drawText("0", area.getRight() + 6.f, area.getCentreY() - 6.f, 28.f, 12.f, juce::Justification::left);
        g.drawText("-1", area.getRight() + 6.f, area.getBottom() - 8.f, 28.f, 12.f, juce::Justification::left);
    }
    g.setColour(green); g.setFont(font(9.f, true));
    g.drawText("L", leftArea.getX() + 5.f, leftArea.getY() + 3.f, 18.f, 12.f, juce::Justification::left);
    g.setColour(juce::Colour(88, 192, 199));
    g.drawText("R", rightArea.getX() + 5.f, rightArea.getY() + 3.f, 18.f, 12.f, juce::Justification::left);
    if (size == 0)
    {
        g.setColour(muted); g.setFont(font(11.f));
        g.drawText("OUTPUT WAVEFORM", leftArea, juce::Justification::centred);
        return;
    }
    struct Column { float loL = 1, hiL = -1, loR = 1, hiR = -1, gr = 0; bool ready = false; };
    std::array<Column, 2048> columns{};
    const int width = juce::jlimit(1, 2048, juce::roundToInt(grArea.getWidth()));
    const double end = static_cast<double>(frames[size - 1].sampleEnd) / frames[size - 1].sampleRate;
    const double window = 6.0;
    for (size_t i = 0; i < size; ++i)
    {
        const auto& f = frames[i];
        const double age = end - static_cast<double>(f.sampleEnd) / f.sampleRate;
        if (age < 0.0 || age > window) continue;
        const int x = juce::jlimit(0, width - 1, static_cast<int>((1.0 - age / window) * (width - 1)));
        auto& c = columns[static_cast<size_t>(x)];
        c.loL = juce::jmin(c.loL, f.minL); c.hiL = juce::jmax(c.hiL, f.maxL);
        c.loR = juce::jmin(c.loR, f.minR); c.hiR = juce::jmax(c.hiR, f.maxR);
        c.gr = juce::jmax(c.gr, f.reduction); c.ready = true;
    }
    juce::Path gr;
    bool started = false;
    for (int x = 0; x < width; ++x)
    {
        const auto& c = columns[static_cast<size_t>(x)];
        if (!c.ready) continue;
        const float px = grArea.getX() + static_cast<float>(x);
        const float y = grArea.getY() + grArea.getHeight() * juce::jlimit(0.f, 24.f, c.gr) / 24.f;
        if (!started) { gr.startNewSubPath(px, y); started = true; } else gr.lineTo(px, y);
        const auto draw = [&](juce::Rectangle<float> area, float low, float high, juce::Colour colour)
        {
            const float top = area.getCentreY() - juce::jlimit(-1.f, 1.f, high) * area.getHeight() * .48f;
            const float bottom = area.getCentreY() - juce::jlimit(-1.f, 1.f, low) * area.getHeight() * .48f;
            g.setColour(colour.withAlpha(.2f)); g.fillRect(px - .7f, top - .7f, 2.4f, juce::jmax(1.4f, bottom - top + 1.4f));
            g.setColour(colour.withAlpha(.85f)); g.drawVerticalLine(juce::roundToInt(px), top, juce::jmax(top + .5f, bottom));
            if (high > 1.f || low < -1.f) { g.setColour(pink); g.fillRect(px, area.getY(), 1.f, 2.f); }
        };
        draw(leftArea, c.loL, c.hiL, green);
        draw(rightArea, c.loR, c.hiR, juce::Colour(88, 192, 199));
    }
    if (started)
    {
        g.setColour(pink.withAlpha(.18f)); g.strokePath(gr, juce::PathStrokeType(5.f));
        g.setColour(pink); g.strokePath(gr, juce::PathStrokeType(1.6f));
    }
}

MasterLimiterAudioProcessorEditor::MasterLimiterAudioProcessorEditor(MasterLimiterAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p), history(p.getSignalHistory())
{
    setupSlider(input, inputL, "INPUT", orange, 0.0);
    setupSlider(ceiling, ceilingL, "LIMIT", green, -1.0);
    setupSlider(lookahead, lookaheadL, "LOOKAHEAD", purple, 5.0);
    setupSlider(release, releaseL, "RELEASE", purple, 100.0);
    setupSlider(link, linkL, "STEREO LINK", purple, 1.0);
    std::array<juce::Slider*, 5> sliders{&input, &ceiling, &lookahead, &release, &link};
    const std::array<const char*, 5> ids{"INPUT", p.getLimitParameterID(), "LOOKAHEAD", "RELEASE", "LINK"};
    for (size_t i = 0; i < sliders.size(); ++i)
        attachments[i] = std::make_unique<Attachment>(processor.getParameters(), ids[i], *sliders[i]);
    legacyLimitBinding = p.usesLegacyLimit();
    input.setLookAndFeel(&inputLookAndFeel);
    input.textFromValueFunction = [](double v) {
        return (v > 0.0001 ? juce::String("+") : juce::String()) + juce::String(v, 2) + " dB";
    };
    input.valueFromTextFunction = [](const juce::String& value) { return value.getDoubleValue(); };
    input.updateText();
    ceiling.setTextValueSuffix(processor.isTruePeakEnabled() ? " dBTP" : " dB");
    lookahead.setTextValueSuffix(" ms");
    release.setTextValueSuffix(" ms");
    link.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    link.valueFromTextFunction = [](const juce::String& s) { return s.getDoubleValue() / 100.0; };
    link.updateText();
    input.setTooltip("Gain before the optional Soft Clip and limiter.");
    ceiling.setTooltip(p.usesLegacyLimit()
        ? "Output sample-peak limit. This saved project retains the original -3 to 0 dB range and automation."
        : "Maximum output level: -6 to 0 dB. TRUE PEAK enables intersample protection.");
    lookahead.setTooltip("Peak anticipation. Additional TP safety delay is compensated by the host and remains the same in both modes.");
    release.setTooltip("Gain recovery time after a peak.");
    link.setTooltip("0%: independent channels. 100%: linked stereo gain.");
    softClipButton.setClickingTogglesState(true);
    softClipButton.setColour(juce::TextButton::buttonColourId, panel2);
    softClipButton.setColour(juce::TextButton::buttonOnColourId, orange.withAlpha(.25f));
    softClipButton.setColour(juce::TextButton::textColourOffId, muted);
    softClipButton.setColour(juce::TextButton::textColourOnId, orange);
    softClipButton.setTooltip("Optional input saturation. Off: clean gain. On: original tanh character. Switching is smoothed over 20 ms.");
    addAndMakeVisible(softClipButton);
    softClipAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.getParameters(), "SOFTCLIP", softClipButton);
    truePeakButton.setClickingTogglesState(true);
    truePeakButton.setColour(juce::TextButton::buttonColourId, panel2);
    truePeakButton.setColour(juce::TextButton::buttonOnColourId, green.withAlpha(.25f));
    truePeakButton.setColour(juce::TextButton::textColourOffId, muted);
    truePeakButton.setColour(juce::TextButton::textColourOnId, green);
    truePeakButton.setTooltip("8x FIR true-peak safety stage with 0.75 dB internal allowance. Same host latency in both modes.");
    addAndMakeVisible(truePeakButton);
    truePeakAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.getParameters(), "TRUEPEAK", truePeakButton);
    addAndMakeVisible(history);
    resetButton.setColour(juce::TextButton::buttonColourId, panel2);
    resetButton.setColour(juce::TextButton::textColourOffId, muted);
    resetButton.onClick = [this] { processor.resetMeters(); history.clear(); };
    addAndMakeVisible(resetButton);
    setResizable(true, true);
    setResizeLimits(900, 580, 1800, 1100);
    setSize(1100, 700);
    startTimerHz(30);
}

MasterLimiterAudioProcessorEditor::~MasterLimiterAudioProcessorEditor()
{
    input.setLookAndFeel(nullptr);
}

void MasterLimiterAudioProcessorEditor::bindLimit()
{
    attachments[1].reset();
    legacyLimitBinding = processor.usesLegacyLimit();
    attachments[1] = std::make_unique<Attachment>(processor.getParameters(), processor.getLimitParameterID(), ceiling);
    ceiling.setTextValueSuffix(processor.isTruePeakEnabled() ? " dBTP" : " dB");
    ceiling.setTooltip(legacyLimitBinding
        ? "Output sample-peak limit. This saved project retains the original -3 to 0 dB range and automation."
        : "Maximum output level: -6 to 0 dB. TRUE PEAK enables intersample protection.");
}

void MasterLimiterAudioProcessorEditor::setupSlider(juce::Slider& s, juce::Label& label,
    const juce::String& name, juce::Colour colour, double defaultValue)
{
    s.setName(name);
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 100, 22);
    s.setDoubleClickReturnValue(true, defaultValue);
    s.setColour(juce::Slider::rotarySliderFillColourId, colour);
    s.setColour(juce::Slider::thumbColourId, colour);
    s.setColour(juce::Slider::rotarySliderOutlineColourId, gridBright);
    s.setColour(juce::Slider::textBoxTextColourId, text);
    s.setColour(juce::Slider::textBoxBackgroundColourId, panel2);
    s.setColour(juce::Slider::textBoxOutlineColourId, grid);
    addAndMakeVisible(s);
    label.setText(name, juce::dontSendNotification);
    label.setFont(font(12.f, true));
    label.setColour(juce::Label::textColourId, muted);
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);
}

void MasterLimiterAudioProcessorEditor::resized()
{
    truePeakButton.setBounds(getWidth() - 514, 24, 108, 28);
    softClipButton.setBounds(getWidth() - 396, 24, 108, 28);
    auto area = getLocalBounds().reduced(26);
    area.removeFromTop(58);
    area.removeFromBottom(22);
    controlsPanel = area.removeFromBottom(180);
    area.removeFromBottom(12);
    metersPanel = area.removeFromRight(250);
    area.removeFromRight(12);
    graphPanel = area;
    history.setBounds(graphPanel.reduced(16).withTrimmedTop(30).withTrimmedBottom(26));
    resetButton.setBounds(metersPanel.getX() + 16, metersPanel.getBottom() - 43, metersPanel.getWidth() - 32, 26);
    auto knobs = controlsPanel.reduced(12, 14);
    const int cellWidth = knobs.getWidth() / 5;
    std::array<juce::Slider*, 5> sliders{&input, &lookahead, &release, &link, &ceiling};
    std::array<juce::Label*, 5> labels{&inputL, &lookaheadL, &releaseL, &linkL, &ceilingL};
    for (size_t i = 0; i < sliders.size(); ++i)
    {
        auto cell = knobs.removeFromLeft(cellWidth);
        labels[i]->setBounds(cell.removeFromTop(22));
        sliders[i]->setBounds(cell.reduced(5, 0));
    }
}

void MasterLimiterAudioProcessorEditor::drawMeter(juce::Graphics& g, juce::Rectangle<float> r,
    float db, const juce::String& name, juce::Colour colour, bool reduction)
{
    g.setFont(font(10.f, true));
    g.setColour(muted);
    g.drawText(name, r.removeFromTop(20.f), juce::Justification::centred);
    auto number = r.removeFromBottom(24.f);
    const float norm = reduction ? juce::jlimit(0.f, 1.f, db / 24.f) : juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
    auto track = r.reduced(15.f, 4.f);
    g.setColour(grid);
    g.fillRoundedRectangle(track, 3.f);
    auto fill = reduction ? track.withHeight(track.getHeight() * norm) : track.withTop(track.getBottom() - track.getHeight() * norm);
    g.setColour(colour);
    g.fillRoundedRectangle(fill, 3.f);
    g.setColour(text);
    g.setFont(font(11.f, true));
    g.drawText(reduction ? "-" + juce::String(db, 1) : dbText(db), number, juce::Justification::centred);
}

void MasterLimiterAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(bg);
    g.setColour(panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(10.f), 16.f);
    g.setColour(grid);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(10.5f), 16.f, 1.f);
    g.setColour(text);
    g.setFont(font(24.f, true));
    g.drawText("MASTER LIMITER PRO", 28, 18, 450, 30, juce::Justification::left);
    g.setColour(muted);
    g.setFont(font(10.f, true));
    g.drawText("MASTERING DYNAMICS / LOOKAHEAD LIMITER", 30, 46, 420, 18, juce::Justification::left);
    g.setColour(green);
    g.fillEllipse(static_cast<float>(getWidth() - 235), 29.f, 8.f, 8.f);
    g.setFont(font(10.5f, true));
    g.drawText((processor.isTruePeakEnabled() ? juce::String("TP 8x") : juce::String("SAMPLE PEAK")) + "  |  " + juce::String(processor.getLatencyMs(), 1) + " ms", getWidth() - 220, 23, 190, 20, juce::Justification::left);
    g.setColour(grid);
    g.drawHorizontalLine(70, 26.f, static_cast<float>(getWidth() - 26));
    box(g, graphPanel); box(g, metersPanel); box(g, controlsPanel);
    g.setColour(muted);
    g.setFont(font(10.f, true));
    g.drawText("GAIN REDUCTION  /  OUTPUT L + R", graphPanel.reduced(16).removeFromTop(18), juce::Justification::left);
    g.setColour(pink);
    g.drawText("6 s", graphPanel.reduced(16).removeFromBottom(18), juce::Justification::right);
    g.setColour(muted);
    g.drawText("LEVELS", metersPanel.reduced(16).removeFromTop(18), juce::Justification::left);
    auto bars = metersPanel.toFloat().reduced(12.f).withTrimmedTop(26.f).withTrimmedBottom(114.f);
    const float width = bars.getWidth() / 3.f;
    drawMeter(g, bars.removeFromLeft(width), inputDb, "INPUT", orange, false);
    drawMeter(g, bars.removeFromLeft(width), reductionDb, "GR", pink, true);
    drawMeter(g, bars, outputDb, "OUTPUT", green, false);
    auto footer = metersPanel.reduced(16).withTop(metersPanel.getBottom() - 117);
    g.setFont(font(10.f));
    g.setColour(muted);
    g.drawText("IN RMS   " + dbText(inputRms), footer.removeFromTop(14), juce::Justification::left);
    g.drawText("OUT RMS   " + dbText(outputRms), footer.removeFromTop(14), juce::Justification::left);
    g.setColour(green);
    g.drawText("OUT HOLD   " + dbText(outputHold), footer.removeFromTop(14), juce::Justification::left);
    const float tp = processor.getOutputTruePeak();
    g.drawText("OUT TP   " + (tp <= -99.f ? juce::String("-inf") : juce::String(tp, 1)) + " dBTP", footer.removeFromTop(14), juce::Justification::left);
    g.setColour(muted);
    g.setFont(font(10.f));
    g.drawText("OUTPUT WAVEFORM  /  6 s  /  RMS + TRUE-PEAK METERING", 28, getHeight() - 36, 500, 18, juce::Justification::left);
}

void MasterLimiterAudioProcessorEditor::timerCallback()
{
    if (legacyLimitBinding != processor.usesLegacyLimit()) bindLimit();
    if ((ceiling.getTextValueSuffix() == " dBTP") != processor.isTruePeakEnabled())
        ceiling.setTextValueSuffix(processor.isTruePeakEnabled() ? " dBTP" : " dB");
    inputDb = processor.getInputPeak(); outputDb = processor.getOutputPeak();
    reductionDb = processor.getGainReduction();
    inputRms = processor.getInputRms(); outputRms = processor.getOutputRms();
    outputHold = processor.getPeakHold();
    history.update();
    repaint();
}
