#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

struct SignalFrame
{
    float minL = 0, maxL = 0, minR = 0, maxR = 0, reduction = 0, limit = 1;
    uint64_t sampleEnd = 0;
    float sampleRate = 48000.f;
};

// One audio writer, arbitrary GUI snapshots. Atomic payloads avoid C++ data
// races; sequentially consistent sequence markers prevent mixed overwritten
// frames. Work is bounded and the audio writer never waits for the reader.
class SignalHistory
{
public:
    static constexpr size_t capacity = 4096;
    void prepare(double sr)
    {
        binSamples = std::max(1, static_cast<int>(std::lround(sr * .002)));
        sampleRate.store(static_cast<float>(sr));
        reset();
    }
    void reset() noexcept
    {
        first.store(next);
        clock = 0; count = 0; partial = {};
    }
    uint64_t published() const noexcept { return completed.load(); }
    float rate() const noexcept { return sampleRate.load(); }
    void push(float left, float right, float gr, float ceiling) noexcept
    {
        if (count == 0) { partial = {}; partial.minL = partial.maxL = left; partial.minR = partial.maxR = right; partial.limit = ceiling; }
        partial.minL = std::min(partial.minL, left); partial.maxL = std::max(partial.maxL, left);
        partial.minR = std::min(partial.minR, right); partial.maxR = std::max(partial.maxR, right);
        partial.reduction = std::max(partial.reduction, gr);
        partial.limit = std::min(partial.limit, ceiling);
        ++clock;
        if (++count < binSamples) return;
        auto& slot = slots[static_cast<size_t>(next % capacity)];
        slot.stamp.store(next * 2 + 1);
        slot.minL.store(partial.minL); slot.maxL.store(partial.maxL);
        slot.minR.store(partial.minR); slot.maxR.store(partial.maxR);
        slot.reduction.store(partial.reduction); slot.limit.store(partial.limit);
        slot.sampleEnd.store(clock);
        slot.rate.store(sampleRate.load());
        slot.stamp.store(next * 2 + 2);
        completed.store(++next);
        count = 0;
    }
    size_t copyLatest(std::array<SignalFrame, capacity>& out, uint64_t after = 0) const noexcept
    {
        const auto beginEpoch = first.load();
        const auto end = completed.load();
        const auto begin = std::max(std::max(beginEpoch, after), end > capacity ? end - capacity : 0);
        size_t countOut = 0;
        for (auto seq = begin; seq < end; ++seq)
        {
            const auto& s = slots[static_cast<size_t>(seq % capacity)];
            const auto expected = seq * 2 + 2;
            if (s.stamp.load() != expected) continue;
            SignalFrame f{s.minL.load(), s.maxL.load(), s.minR.load(), s.maxR.load(), s.reduction.load(), s.limit.load(), s.sampleEnd.load(), s.rate.load()};
            if (s.stamp.load() == expected) out[countOut++] = f;
        }
        return first.load() == beginEpoch ? countOut : 0;
    }
private:
    struct Slot
    {
        std::atomic<uint64_t> stamp{0}, sampleEnd{0};
        std::atomic<float> minL{0}, maxL{0}, minR{0}, maxR{0}, reduction{0}, limit{1}, rate{48000};
    };
    std::array<Slot, capacity> slots;
    std::atomic<uint64_t> completed{0}, first{0};
    std::atomic<float> sampleRate{48000};
    uint64_t next = 0, clock = 0;
    int binSamples = 96, count = 0;
    SignalFrame partial;
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
