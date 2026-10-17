#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Fixed audio delay keeps host compensation stable under lookahead automation.
// A preallocated range-maximum tree queries the exact future peak interval,
// including the output sample itself. No allocations or scans of the full
// lookahead window take place in process().
class LookaheadLimiter
{
public:
    void prepare(double sampleRate, int delayOverride = -1)
    {
        delay = delayOverride >= 0 ? delayOverride : static_cast<int>(std::lround(sampleRate * .020));
        capacity = 1;
        while (capacity <= delay) capacity *= 2;
        for (auto& channel : channels)
        {
            channel.audio.assign(static_cast<size_t>(capacity), 0.f);
            channel.tree.assign(static_cast<size_t>(capacity * 2), 0.f);
            channel.gain = 1.f;
        }
        writeIndex = 0;
    }
    void reset() noexcept
    {
        for (auto& channel : channels)
        {
            std::fill(channel.audio.begin(), channel.audio.end(), 0.f);
            std::fill(channel.tree.begin(), channel.tree.end(), 0.f);
            channel.gain = 1.f;
        }
        writeIndex = 0;
    }
    int latency() const noexcept { return delay; }
    struct Frame { std::array<float, 2> audio; float reduction; };
    Frame process(float left, float right, int anticipation, float ceiling,
                  float releaseCoefficient, float link) noexcept
    {
        anticipation = std::clamp(anticipation, 0, delay);
        const int readIndex = (writeIndex + capacity - delay) & (capacity - 1);
        const int end = (readIndex + anticipation) & (capacity - 1);
        std::array<float, 2> peaks{};
        const std::array<float, 2> input{left, right};
        for (size_t c = 0; c < 2; ++c)
        {
            auto& channel = channels[c];
            channel.audio[static_cast<size_t>(writeIndex)] = input[c];
            int node = capacity + writeIndex;
            channel.tree[static_cast<size_t>(node)] = std::abs(input[c]);
            while ((node /= 2) > 0)
                channel.tree[static_cast<size_t>(node)] = std::max(channel.tree[static_cast<size_t>(node * 2)], channel.tree[static_cast<size_t>(node * 2 + 1)]);
            peaks[c] = end >= readIndex ? maximum(channel, readIndex, end)
                : std::max(maximum(channel, readIndex, capacity - 1), maximum(channel, 0, end));
        }
        const std::array<float, 2> ownGain{peaks[0] > ceiling ? ceiling / peaks[0] : 1.f,
                                         peaks[1] > ceiling ? ceiling / peaks[1] : 1.f};
        const float linkedGain = std::min(ownGain[0], ownGain[1]);
        Frame result{{}, 0.f};
        for (size_t c = 0; c < 2; ++c)
        {
            auto& channel = channels[c];
            const float target = ownGain[c] + link * (linkedGain - ownGain[c]);
            channel.gain = target < channel.gain ? target
                : releaseCoefficient * channel.gain + (1.f - releaseCoefficient) * target;
            // Numerical guard only: the queried peak interval guarantees this
            // bound analytically, including with changing lookahead and link.
            result.audio[c] = std::clamp(channel.audio[static_cast<size_t>(readIndex)] * channel.gain, -ceiling, ceiling);
            result.reduction = std::max(result.reduction, -20.f * std::log10(std::max(channel.gain, 1.e-8f)));
        }
        writeIndex = (writeIndex + 1) & (capacity - 1);
        return result;
    }
private:
    struct Channel { std::vector<float> audio, tree; float gain = 1.f; };
    std::array<Channel, 2> channels;
    int capacity = 1, delay = 0, writeIndex = 0;
    float maximum(const Channel& c, int first, int last) const noexcept
    {
        float peak = 0.f;
        for (first += capacity, last += capacity; first <= last; first /= 2, last /= 2)
        {
            if ((first & 1) != 0) peak = std::max(peak, c.tree[static_cast<size_t>(first++)]);
            if ((last & 1) == 0) peak = std::max(peak, c.tree[static_cast<size_t>(last--)]);
        }
        return peak;
    }
};
