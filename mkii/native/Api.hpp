#pragma once

#include <cstdint>

namespace mkii::native {

struct Controls {
    float threshold=0.2f, ratio=4, makeup=0;
};

// Only POD values and opaque pointers cross the Windows compiler boundary.
struct Api {
    std::uint32_t version;
    int latency;
    void* (*create)();
    void (*destroy)(void*);
    int (*prepare)(void*,double,int);
    void (*stop)(void*);
    int (*process)(void*,float* const*,int,const Controls*,int,int);
    unsigned (*failedSolves)(void*);
    float (*controlVolts)(void*);
};
constexpr std::uint32_t apiVersion=1;
using LoadApi=const Api* (*)();

}
