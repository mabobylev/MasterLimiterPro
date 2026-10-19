#include "LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace { bool countAllocations = false; std::size_t allocations = 0; }
void* operator new(std::size_t n) { if (countAllocations) ++allocations; if (auto* p=std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { if (countAllocations) ++allocations; if (auto* p=std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr double pi = 3.14159265358979323846;
int failures=0;
void check(bool ok, const char* label) { if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; } }
bool near(float a, float b, float tolerance) { return std::isfinite(a) && std::abs(a-b)<=tolerance; }

void tone(LoudnessMeter& meter, int rate, int samples, float amplitude=0.25f, int channels=2, bool anti=false)
{
    for (int i=0;i<samples;++i) {
        const float x=amplitude*static_cast<float>(std::sin(2*pi*1000.0*i/rate));
        meter.process(x, anti ? -x : x, channels);
    }
}

int runTests()
{
    LoudnessMeter meter;
    meter.prepare(48000.0);
    check(meter.readings().momentary==-100.0f && meter.readings().shortTerm==-100.0f && meter.readings().integrated==-100.0f, "initial sentinel");
    tone(meter,48000,19199);
    check(meter.readings().momentary==-100.0f && meter.readings().integrated==-100.0f, "momentary remains sentinel before 400 ms");
    meter.process(0.0f,0.0f,2);
    check(meter.readings().momentary>-100.0f && meter.readings().integrated>-100.0f, "momentary and integrated start at full 400 ms block");
    check(meter.readings().shortTerm==-100.0f, "short term remains sentinel before 3 s");
    tone(meter,48000,3*48000-19200);
    check(meter.readings().shortTerm>-100.0f, "short term appears at 3 s");

    meter.reset(); tone(meter,48000,48000,0.2f,1);
    const float mono=meter.readings().integrated;
    meter.reset(); tone(meter,48000,48000,0.2f,2);
    const float stereo=meter.readings().integrated;
    check(near(stereo-mono,3.0103f,0.02f), "stereo channel energies sum for +3.0103 LU over mono");
    meter.reset(); tone(meter,48000,48000,0.2f,2,true);
    check(near(meter.readings().integrated,stereo,0.001f), "antiphase does not cancel");

    meter.reset(); tone(meter,48000,10*48000,0.2f);
    const float loud=meter.readings().integrated;
    tone(meter,48000,10*48000,0.02f);
    check(near(meter.readings().integrated,loud,0.08f), "relative gate excludes quiet blocks");
    meter.reset(); tone(meter,48000,2*48000,0.0001f);
    check(meter.readings().integrated==-100.0f, "absolute gate excludes blocks below -70 LUFS");
    meter.reset();
    check(meter.readings().momentary==-100.0f && meter.readings().shortTerm==-100.0f && meter.readings().integrated==-100.0f, "reset clears readings");

    for (const int rate : {44100,48000,96000,192000,384000}) {
        meter.prepare(rate); tone(meter,rate,rate,0.25f,2,true);
        const auto v=meter.readings();
        check(std::isfinite(v.momentary) && std::isfinite(v.integrated) && v.shortTerm==-100.0f, "sample-rate matrix finite and short window sentinel");
        if (rate==48000)
            check(near(v.integrated,-12.035f,0.03f), "48 kHz 1 kHz antiphase calibration level");
    }
    LoudnessMeter partitionA, partitionB;
    partitionA.prepare(48000); partitionB.prepare(48000);
    for (int i=0;i<48000;++i) {
        const float x=0.3f*static_cast<float>(std::sin(2*pi*997.0*i/48000.0));
        partitionA.process(x,-x,2);
    }
    int position=0; const int chunks[]={1,17,257,1024}; int chunk=0;
    while(position<48000) {
        const int end=std::min(48000,position+chunks[chunk++%4]);
        for(;position<end;++position) {
            const float x=0.3f*static_cast<float>(std::sin(2*pi*997.0*position/48000.0));
            partitionB.process(x,-x,2);
        }
    }
    check(near(partitionA.readings().momentary,partitionB.readings().momentary,0.001f) &&
          near(partitionA.readings().integrated,partitionB.readings().integrated,0.001f), "sample result is invariant to caller block partitioning");
    meter.prepare(48000);
    meter.process(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),2);
    for(int i=0;i<48000;++i) meter.process(1.0e30f,-1.0e30f,2);
    check(std::isfinite(meter.readings().integrated), "nonfinite and extreme finite samples are contained");

    meter.prepare(48000);
    allocations=0; countAllocations=true;
    for(int i=0;i<100000;++i) meter.process(0.1f,-0.1f,2);
    const auto snapshot=meter.readings(); (void)snapshot;
    meter.reset();
    countAllocations=false;
    check(allocations==0, "process/readings/reset allocate nothing");
    return failures ? 1 : 0;
}

int readFixture(int argc,char** argv)
{
    if(argc!=5 || std::string(argv[1])!="--read-f32") return -1;
    const int rate=std::stoi(argv[3]), channels=std::stoi(argv[4]);
    if(rate<8000 || channels<1 || channels>2) return 2;
    std::ifstream input(argv[2],std::ios::binary);
    if(!input) { std::cerr << "cannot open raw float32 input\n"; return 2; }
    LoudnessMeter meter; meter.prepare(rate);
    float sample[2]{};
    while(input.read(reinterpret_cast<char*>(sample),channels*sizeof(float)))
        meter.process(sample[0],channels==2?sample[1]:0.0f,channels);
    if(!input.eof() || input.gcount()!=0) { std::cerr << "truncated raw float32 input\n"; return 2; }
    const auto r=meter.readings();
    std::cout << "{\"momentary\":"<<r.momentary<<",\"short_term\":"<<r.shortTerm<<",\"integrated\":"<<r.integrated<<"}\n";
    return 0;
}
}

int main(int argc,char** argv)
{
    const int fixture=readFixture(argc,argv);
    if(fixture>=0) return fixture;
    if(argc!=1) { std::cerr << "usage: MasterLimiterLoudnessTests [--read-f32 RAW_PATH SAMPLE_RATE CHANNELS]\n"; return 2; }
    return runTests();
}
