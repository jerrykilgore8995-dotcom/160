#pragma once

#include <cstdint>

namespace mkii {
constexpr std::uint32_t wireMagic = 0x16000101;
constexpr std::uint32_t maxBlock = 4096;

// Local child-process protocol. Both ends use the same x64 little-endian ABI.
struct Start {
    std::uint32_t magic = wireMagic;
    std::uint32_t reserved = 0;
    double rate = 44100;
};

struct Block {
    std::uint32_t samples = 0;
    float thresholdVrms = 0.1f;
    float ratio = 4.0f;
    float makeupDb = 0.0f;
};

struct Reply {
    std::uint32_t samples = 0;
    float controlVolts = 0.0f;
};
static_assert(sizeof(Start) == 16 && sizeof(Block) == 16 && sizeof(Reply) == 8);

// Differential peak volts for a full scale sample. This maps a -18 dBFS
// peak sine to +4 dBu RMS at the deck's balanced input.
constexpr double fullScalePeakVolts = 13.790836925122916;
constexpr double settleSeconds = 0.05;
constexpr double maxStep = 2.5e-6;
}
