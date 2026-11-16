#include "Pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <thread>

namespace {
using mkii::native::Solver;
constexpr int blockSize=128;
constexpr int frames=44160;
constexpr double targetRate=44100;

struct Result {
    double seconds=0,p99ms=0,peakms=0,rms=0;
    unsigned misses=0,late=0;
};

double signal(int n,double rate,bool stress,int channel) {
    const double t=n/rate, angle=2*std::numbers::pi*t;
    if (!stress) return 0.09*std::sin(1000*angle+channel);
    const double envelope=(t>0.28 && t<0.43)?0.8:0.22;
    if (n%blockSize==60 && (n/blockSize)%41==3) return 0.75;
    return envelope*(0.57*std::sin(77*angle+channel)+0.19*std::sin(593*angle)
                     +0.11*std::sin(3241*angle)+0.08*std::sin(6771*angle)
                     +0.05*std::sin(12949*angle));
}

Result measure(double rate,bool stress,int channel) {
    Solver solver;
    Result result;
    if (!solver.prepare(rate)) {
        result.misses=frames;
        return result;
    }
    std::array<double,frames/blockSize> milliseconds{};
    const auto start=std::chrono::steady_clock::now();
    for (int block=0;block<frames/blockSize;++block) {
        const auto now=std::chrono::steady_clock::now();
        for (int n=0;n<blockSize;++n) {
            const double in=signal(block*blockSize+n,rate,stress,channel);
            const double y=solver.step(in,0.2,4,0);
            result.rms+=y*y;
        }
        const auto elapsed=std::chrono::steady_clock::now()-now;
        milliseconds[block]=std::chrono::duration<double,std::milli>(elapsed).count();
        result.late+=milliseconds[block]>(blockSize*1000/rate);
    }
    result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    result.misses=solver.failures();
    result.rms=std::sqrt(result.rms/frames);
    std::sort(milliseconds.begin(),milliseconds.end());
    result.p99ms=milliseconds[milliseconds.size()*99/100];
    result.peakms=milliseconds.back();
    return result;
}

bool playback(double rate,int channels) {
    mkii::native::Pipeline engine;
    if (!engine.prepare(rate,channels)) return false;
    using Clock=std::chrono::steady_clock;
    const auto begin=Clock::now();
    double peakMs=0,energy=0;
    unsigned lateCallbacks=0;
    std::array<std::array<float,blockSize>,2> audio{};
    std::array<float*,2> pointers{audio[0].data(),audio[1].data()};
    const int blocks=static_cast<int>(3.0*rate/blockSize);
    for (int b=0;b<blocks;++b) {
        for (int c=0;c<channels;++c)
            for (int n=0;n<blockSize;++n)
                audio[c][n]=static_cast<float>(signal(b*blockSize+n,rate,true,c));
        const auto scheduled=begin+std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(b*blockSize/rate));
        std::this_thread::sleep_until(scheduled);
        const auto callback=Clock::now();
        engine.process(pointers.data(),blockSize,{});
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-callback).count();
        peakMs=std::max(peakMs,ms);
        lateCallbacks+=ms>blockSize*1000/rate;
        for (int c=0;c<channels;++c)
            for (auto v : audio[c]) energy+=v*v;
    }
    const auto missed=engine.missedSamples();
    const auto failures=engine.failedSolves();
    std::printf("%.0f Hz, %d channel(s), 128-sample callback max %.3f ms, "
                "late callbacks %u, late samples %llu, failed solves %u, output energy %.6f\n",
                rate,channels,peakMs,lateCallbacks,static_cast<unsigned long long>(missed),failures,energy);
    return !lateCallbacks && !missed && !failures && std::isfinite(energy) && energy>0.1;
}

int capture() {
    Solver solver;
    if (!solver.prepare(targetRate)) return 2;
    for (int n=0;n<4410;++n) {
        const float in=static_cast<float>(signal(n,targetRate,false,0));
        const double out=solver.step(in,0.2,4,0);
        std::printf("%.15g %.15g %.15g\n",out,solver.controlVolts(),solver.levelVolts());
    }
    return solver.failures() ? 3 : 0;
}
}

int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--capture")==0) return capture();
    const bool gate=argc==2 && std::strcmp(argv[1],"--gate")==0;
    if (argc==2 && std::strcmp(argv[1],"--playback")==0) {
        bool pass=true;
        for (const double rate : {44100.,48000.})
            for (const int channels : {1,2}) pass=playback(rate,channels) && pass;
        return pass?0:4;
    }
    bool pass=true;
    for (const double rate : {44100.,48000.}) {
        for (const bool stress : {false,true}) {
            Result stereoRight;
            std::thread thread([&]{stereoRight=measure(rate,stress,1);});
            const auto left=measure(rate,stress,0);
            thread.join();
            int channel=0;
            for (const auto& r : {left,stereoRight}) {
                const bool qualifies=r.misses==0 && r.late==0 && r.rms>0.0005;
                pass=pass && qualifies;
                std::printf("%.0f Hz, %s, channel %d: %.3f s for %.3f s of audio, "
                            "128-sample p99 %.3f ms, max %.3f ms, missed deadlines %u, "
                            "failed solves %u, RMS %.8f\n",rate,stress?"transient":"1 kHz",++channel,
                            r.seconds,frames/rate,r.p99ms,r.peakms,r.late,r.misses,r.rms);
            }
        }
    }
    std::printf("Real-time check: %s\n",pass?"pass":"fail");
    return gate && !pass ? 1 : 0;
}
