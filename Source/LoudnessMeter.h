#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Standalone BS.1770-style loudness meter. prepare() may allocate; process(),
// reset(), and readings() never allocate or lock. The caller owns thread access.
class LoudnessMeter
{
public:
    struct Readings { float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f; };

    void prepare(double sampleRate)
    {
        sampleRate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 960000.0
                    ? sampleRate : 48000.0;
        updateSamples_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sampleRate_ * 0.1)));
        reset();
        designFilters();
    }

    void reset() noexcept
    {
        energyBuckets_.fill(0.0);
        writeBucket_ = bucketCount_ = sinceUpdate_ = 0;
        currentBucketEnergy_ = 0.0;
        resetFilter(leftShelf_); resetFilter(leftHigh_); resetFilter(rightShelf_); resetFilter(rightHigh_);
        histogramEnergy_.fill(0.0); histogramCount_.fill(0);
        values_ = {};
    }

    void process(float left, float right, int channels = 2) noexcept
    {
        const double l = sanitize(left);
        const double r = channels > 1 ? sanitize(right) : 0.0;
        const double yl = highPass(leftHigh_, shelf(leftShelf_, l));
        const double yr = channels > 1 ? highPass(rightHigh_, shelf(rightShelf_, r)) : 0.0;
        const double energy = yl * yl + yr * yr;
        currentBucketEnergy_ += energy;
        if (++sinceUpdate_ >= updateSamples_)
        {
            sinceUpdate_ = 0;
            energyBuckets_[writeBucket_] = currentBucketEnergy_;
            currentBucketEnergy_ = 0.0;
            writeBucket_ = (writeBucket_ + 1) % energyBuckets_.size();
            if (bucketCount_ < energyBuckets_.size()) ++bucketCount_;
            if (bucketCount_ >= 4)
            {
                double sum = 0.0;
                for (std::size_t i=0; i<4; ++i)
                    sum += energyBuckets_[(writeBucket_ + energyBuckets_.size() - 1 - i) % energyBuckets_.size()];
                const double mean = sum / static_cast<double>(4 * updateSamples_);
                values_.momentary = lufs(mean);
                addIntegratedBlock(mean);
                values_.integrated = integratedLufs();
            }
            if (bucketCount_ >= energyBuckets_.size())
            {
                double sum = 0.0;
                for (const double bucket : energyBuckets_) sum += bucket;
                values_.shortTerm = lufs(sum / static_cast<double>(energyBuckets_.size() * updateSamples_));
            }
        }
    }

    Readings readings() const noexcept { return values_; }

private:
    static constexpr std::size_t binCount_ = 15001; // -70.00 through +80.00 LUFS
    struct Biquad { double b0=1, b1=0, b2=0, a1=0, a2=0, z1=0, z2=0; };
    double sampleRate_ = 48000.0;
    std::size_t updateSamples_=4800;
    std::array<double, 30> energyBuckets_{};
    std::size_t writeBucket_=0, bucketCount_=0, sinceUpdate_=0;
    double currentBucketEnergy_=0.0;
    Biquad leftShelf_, leftHigh_, rightShelf_, rightHigh_;
    std::array<double, binCount_> histogramEnergy_{};
    std::array<std::uint64_t, binCount_> histogramCount_{};
    Readings values_;

    static double sanitize(float x) noexcept
    {
        if (!std::isfinite(x)) return 0.0;
        return static_cast<double>(x);
    }
    static void resetFilter(Biquad& f) noexcept { f.z1 = f.z2 = 0.0; }
    void designFilters() noexcept
    {
        // Annex 1 BS.1770 K-weighting shelf and RLB high-pass bilinear
        // transforms. Coefficients are prepared off-thread.
        constexpr double shelfHz=1681.974450955533, shelfGain=3.999843853973347, shelfQ=0.7071752369554196;
        constexpr double highHz=38.13547087602444, highQ=0.5003270373238773;
        constexpr double vbExponent=0.4996667741545416;
        const double shelfK=std::tan(3.14159265358979323846*shelfHz/sampleRate_);
        const double shelfK2=shelfK*shelfK;
        const double shelfVh=std::pow(10.0,shelfGain/20.0);
        const double shelfVb=std::pow(shelfVh,vbExponent);
        const double shelfA0=1.0+shelfK/shelfQ+shelfK2;
        leftShelf_={(shelfVh+shelfVb*shelfK/shelfQ+shelfK2)/shelfA0,
                    2.0*(shelfK2-shelfVh)/shelfA0,
                    (shelfVh-shelfVb*shelfK/shelfQ+shelfK2)/shelfA0,
                    2.0*(shelfK2-1.0)/shelfA0,
                    (1.0-shelfK/shelfQ+shelfK2)/shelfA0, 0, 0};
        const double highK=std::tan(3.14159265358979323846*highHz/sampleRate_);
        const double highK2=highK*highK;
        const double highA0=1.0+highK/highQ+highK2;
        leftHigh_={1.0, -2.0, 1.0,
                   2.0*(highK2-1.0)/highA0, (1.0-highK/highQ+highK2)/highA0, 0, 0};
        rightShelf_=leftShelf_; rightHigh_=leftHigh_;
    }
    static double shelf(Biquad& f, double x) noexcept
    { const double y=f.b0*x+f.z1; f.z1=f.b1*x-f.a1*y+f.z2; f.z2=f.b2*x-f.a2*y; return y; }
    static double highPass(Biquad& f, double x) noexcept
    { const double y=f.b0*x+f.z1; f.z1=f.b1*x-f.a1*y+f.z2; f.z2=f.b2*x-f.a2*y; return y; }
    static float lufs(double energy) noexcept
    { return energy > 0.0 && std::isfinite(energy) ? static_cast<float>(-0.691 + 10.0*std::log10(energy)) : -100.0f; }
    void addIntegratedBlock(double energy) noexcept
    {
        const double level=lufs(energy);
        if (level <= -70.0f) return;
        const auto index=static_cast<std::size_t>(std::clamp(std::floor((level+70.0)*100.0), 0.0, 15000.0));
        histogramEnergy_[index]+=energy; ++histogramCount_[index];
    }
    float integratedLufs() const noexcept
    {
        double sum=0.0; std::uint64_t n=0;
        for (std::size_t i=0; i<binCount_; ++i) { sum+=histogramEnergy_[i]; n+=histogramCount_[i]; }
        if (!n) return -100.0f;
        const double ungated=sum/static_cast<double>(n);
        const double gate=std::max(std::pow(10.0,(-70.0+0.691)/10.0), ungated*0.1);
        sum=0.0; n=0;
        for (std::size_t i=0; i<binCount_; ++i)
            if (histogramCount_[i] && histogramEnergy_[i]/static_cast<double>(histogramCount_[i]) > gate)
            { sum+=histogramEnergy_[i]; n+=histogramCount_[i]; }
        return n ? lufs(sum/static_cast<double>(n)) : -100.0f;
    }
};
