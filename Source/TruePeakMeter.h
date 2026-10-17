#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

// 8-phase, 64-tap Blackman-windowed sinc reconstruction. Estimate, not a
// certification claim. A longer independently implemented 16x oracle is used
// in the tests. Delayed by 32 base samples; state is continuous across blocks.
class TruePeakMeter
{
public:
    TruePeakMeter()
    {
        constexpr double pi = 3.14159265358979323846;
        for (size_t phase = 0; phase < 8; ++phase)
        {
            double sum = 0;
            for (size_t tap = 0; tap < 64; ++tap)
            {
                const double x = 31.0 + static_cast<double>(phase) / 8.0 - static_cast<double>(tap);
                const double window = .42 - .5 * std::cos(2 * pi * static_cast<double>(tap) / 63.0) + .08 * std::cos(4 * pi * static_cast<double>(tap) / 63.0);
                const double value = (std::abs(x) < 1.e-12 ? 1.0 : std::sin(pi * x) / (pi * x)) * window;
                coefficients[phase][tap] = static_cast<float>(value); sum += value;
            }
            for (auto& v : coefficients[phase]) v = static_cast<float>(v / sum);
        }
    }
    void reset() noexcept { for (auto& c : history) c.fill(0.f); position = 0; }
    float process(float left, float right) noexcept
    {
        history[0][position] = history[0][position + 64] = left;
        history[1][position] = history[1][position + 64] = right;
        position = (position + 1) & 63;
        float peak = std::max(std::abs(left), std::abs(right));
        for (size_t c = 0; c < 2; ++c)
            for (const auto& phase : coefficients)
            {
                double value = 0;
                for (size_t t = 0; t < 64; ++t) value += static_cast<double>(history[c][position + t]) * phase[t];
                peak = std::max(peak, static_cast<float>(std::min(std::abs(value), static_cast<double>(std::numeric_limits<float>::max()))));
            }
        return peak;
    }
private:
    std::array<std::array<float, 64>, 8> coefficients{};
    std::array<std::array<float, 128>, 2> history{};
    size_t position = 0;
};
