#pragma once

#include "Circuit.hpp"
#include "Api.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mkii::native {

// Each channel owns its circuit state and a single-producer, single-consumer ring.
// The callback submits and reads samples without locking or waiting in live mode.
class Pipeline {
public:
    static constexpr int latency = 512;
    static constexpr std::uint64_t capacity = 16384;
    static_assert((capacity & (capacity-1))==0);

    ~Pipeline() {stop();}
    Pipeline()=default;
    Pipeline(const Pipeline&)=delete;
    Pipeline& operator=(const Pipeline&)=delete;

    bool prepare(double rate,int channels,int delaySamples=latency) {
        stop();
        if (channels<1 || channels>2 || delaySamples<128 || delaySamples>=static_cast<int>(capacity)-512)
            return false;
        delay=static_cast<std::uint64_t>(delaySamples);
        for (int i=0;i<channels;++i) {
            auto& v=voices[i];
            if (!v.solver.prepare(rate) || !v.prepareWake()) return false;
            v.written.store(0,std::memory_order_relaxed);
            v.completed.store(0,std::memory_order_relaxed);
            v.failed.store(0,std::memory_order_relaxed);
        }
        try {
            for (;active<channels;++active) {
                auto& v=voices[active];
                v.worker=std::thread([&v]{v.run();});
            }
        } catch (...) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        for (int i=0;i<active;++i) {
            auto& published=voices[i].written;
            published.store(stopSignal,std::memory_order_release);
            voices[i].wake();
        }
        for (int i=0;i<active;++i)
            if (voices[i].worker.joinable()) voices[i].worker.join();
        active=0;
        position=0;
        underruns=0;
        maxRequiredDelay=0;
        control=0;
    }

    bool process(float* const* channels,int count,Controls settings,bool waitForRender=false,bool dry=false) {
        if (active==0 || count<=0) return false;
        bool complete=true;
        // Chunk big offline blocks so their size cannot exhaust the ring.
        for (int offset=0;offset<count;) {
            const int frames=std::min(count-offset,512);
            for (int c=0;c<active;++c) {
                auto& v=voices[c];
                if (waitForRender) {
                    while (position+frames-v.completed.load(std::memory_order_acquire)>capacity)
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                if (position+frames-v.completed.load(std::memory_order_acquire)>capacity) {
                    for (int k=0;k<active;++k) std::fill_n(channels[k]+offset,count-offset,0.0f);
                    underruns+=count-offset;
                    return false;
                }
                for (int i=0;i<frames;++i) {
                    auto& sample=v.input[(position+i)&(capacity-1)];
                    sample={channels[c][offset+i],settings};
                }
            }
            for (int c=0;c<active;++c) {
                auto& published=voices[c].written;
                published.store(position+frames,std::memory_order_release);
                voices[c].wake();
            }
            control=0;
            for (int c=0;c<active;++c) {
                auto& v=voices[c];
                const auto need=position+frames>delay ? position+frames-delay : 0;
                if (waitForRender) {
                    while (v.completed.load(std::memory_order_acquire)<need)
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                const auto available=v.completed.load(std::memory_order_acquire);
                if (!waitForRender)
                    maxRequiredDelay=std::max(maxRequiredDelay,position+frames-available);
                for (int i=0;i<frames;++i) {
                    if (position+i<delay) {
                        channels[c][offset+i]=0;
                        continue;
                    }
                    const auto index=position+i-delay;
                    if (dry) {
                        channels[c][offset+i]=v.input[index&(capacity-1)].value;
                    } else if (index>=available) {
                        // Maintain timeline and latency on an isolated miss.
                        channels[c][offset+i]=v.input[index&(capacity-1)].value;
                        ++underruns;
                        complete=false;
                    } else {
                        const auto& sample=v.output[index&(capacity-1)];
                        channels[c][offset+i]=sample.value;
                        control=std::max(control,sample.control);
                    }
                }
            }
            position+=frames;
            offset+=frames;
        }
        return complete;
    }

    std::uint64_t missedSamples() const {return underruns;}
    std::uint64_t requiredDelaySamples() const {return maxRequiredDelay;}
    unsigned failedSolves() const {
        unsigned count=0;
        for (int i=0;i<active;++i) count+=voices[i].failed.load(std::memory_order_relaxed);
        return count;
    }
    float controlVolts() const {return control;}

private:
    static constexpr auto stopSignal=std::numeric_limits<std::uint64_t>::max();
    struct Input {float value=0;Controls settings;};
    struct Output {float value=0,control=0;};
    struct Voice {
        ~Voice() {
#ifdef _WIN32
            if (event) CloseHandle(event);
#endif
        }
        bool prepareWake() {
#ifdef _WIN32
            if (!event) event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            return event!=nullptr;
#else
            return true;
#endif
        }
        void wake() {
#ifdef _WIN32
            SetEvent(event);
#endif
        }
        void waitForInput() {
#ifdef _WIN32
            WaitForSingleObject(event,INFINITE);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(75));
#endif
        }
        void run() {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
#endif
            std::uint64_t next=0;
            while (true) {
                const auto published=written.load(std::memory_order_acquire);
                if (published==stopSignal) return;
                if (published==next) {
                    waitForInput();
                    continue;
                }
                const auto end=std::min(next+8,published);
                for (;next<end;++next) {
                    const auto& s=input[next&(capacity-1)];
                    const auto& k=s.settings;
                    auto& out=output[next&(capacity-1)];
                    out.value=static_cast<float>(solver.step(s.value,k.threshold,k.ratio,k.makeup));
                    out.control=static_cast<float>(solver.controlVolts());
                }
                failed.store(solver.failures(),std::memory_order_relaxed);
                completed.store(next,std::memory_order_release);
            }
        }
        Solver solver;
        std::array<Input,capacity> input{};
        std::array<Output,capacity> output{};
        alignas(64) std::atomic<std::uint64_t> written{0};
        alignas(64) std::atomic<std::uint64_t> completed{0};
        std::atomic<unsigned> failed{0};
        std::thread worker;
#ifdef _WIN32
        HANDLE event=nullptr;
#endif
    };
    std::array<Voice,2> voices;
    int active=0;
    std::uint64_t position=0,underruns=0,delay=latency,maxRequiredDelay=0;
    float control=0;
};
}
