#include "Wire.hpp"
#include <ngspice/sharedspice.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace mkii;

template <class T>
bool read(T* data, std::size_t count = 1) {
    return std::fread(data, sizeof(T), count, stdin) == count;
}

template <class T>
bool write(const T* data, std::size_t count = 1) {
    return std::fwrite(data, sizeof(T), count, stdout) == count;
}

int command(const std::string& text) {
    auto mutableText = text;
    return ngSpice_Command(mutableText.data());
}

struct Stream {
    Start start;
    Block block;
    std::array<float, maxBlock> input{}, output{};
    std::uint64_t first = 0;
    std::uint32_t produced = 0;
    double previousInput = 0.0;
    double lastTime = 0.0, lastOutput = 0.0;
    int timeIndex = -1, outputIndex = -1, controlIndex = -1;

    bool receive() {
        if (!read(&block) || block.samples == 0)
            return false;
        if (block.samples > maxBlock
            || !std::isfinite(block.thresholdVrms) || block.thresholdVrms <= 0
            || !std::isfinite(block.ratio) || block.ratio < 1 || block.ratio > 120
            || !std::isfinite(block.makeupDb))
            std::exit(2);
        if (!read(input.data(), block.samples))
            std::exit(2);
        for (std::uint32_t i = 0; i < block.samples; ++i)
            if (!std::isfinite(input[i])) input[i] = 0;
        return true;
    }

    static int message(char* text, int, void*) {
        if (std::strstr(text, "Error") || std::strstr(text, "failed")
                                        || std::strstr(text, "Timestep too small"))
            std::fprintf(stderr, "%s\n", text);
        return 0;
    }

    static int exitRequested(int status, bool, bool, int, void*) {
        std::exit(status == 0 ? 3 : status);
    }

    static int initialise(pvecinfoall plot, int, void* user) {
        auto& s = *static_cast<Stream*>(user);
        for (int i = 0; i < plot->veccount; ++i) {
            const std::string name = plot->vecs[i]->vecname;
            if (name == "time") s.timeIndex = i;
            if (name == "out_plus" || name == "v(out_plus)") s.outputIndex = i;
            if (name == "ec_minus" || name == "v(ec_minus)") s.controlIndex = i;
        }
        if (s.timeIndex < 0 || s.outputIndex < 0 || s.controlIndex < 0)
            std::exit(3);
        return 0;
    }

    double frameAt(double seconds) const {
        return (seconds - settleSeconds) * start.rate;
    }

    static int voltage(double* value, double time, char* name, int, void* user) {
        auto& s = *static_cast<Stream*>(user);
        const double local = s.frameAt(time) - static_cast<double>(s.first);
        if (std::strcmp(name, "vinp") == 0 || std::strcmp(name, "vinm") == 0) {
            const double frame = std::clamp(local, -1.0, static_cast<double>(s.block.samples - 1));
            const int lower = static_cast<int>(std::floor(frame));
            const double low = lower < 0 ? s.previousInput : s.input[lower];
            const double high = s.input[std::min<int>(lower + 1, s.block.samples - 1)];
            *value = std::lerp(low, high, frame - lower) * fullScalePeakVolts
                     * (name[3] == 'p' ? 0.5 : -0.5);
        } else {
            std::exit(3);
        }
        return 0;
    }

    static int sync(double time, double* step, double, int, int, int, void* user) {
        auto& s = *static_cast<Stream*>(user);
        const double frame = s.frameAt(time);
        if (frame > -2) {
            const double nextFrame = std::floor(frame + 1e-6) + 1.0;
            const double boundary = settleSeconds + nextFrame / s.start.rate;
            *step = std::min(*step, boundary - time);
        }
        return 0;
    }

    static int sample(pvecvaluesall point, int, int, void* user) {
        auto& s = *static_cast<Stream*>(user);
        const double time = point->vecsa[s.timeIndex]->creal;
        const double out = point->vecsa[s.outputIndex]->creal;
        const double control = point->vecsa[s.controlIndex]->creal;
        if (!std::isfinite(out) || !std::isfinite(control))
            std::exit(3);
        while (s.produced < s.block.samples) {
            const double target = settleSeconds + static_cast<double>(s.first + s.produced) / s.start.rate;
            if (target > time + 1e-12)
                break;
            const double timeSpan = time - s.lastTime;
            const double mix = timeSpan > 0
                ? std::clamp((target - s.lastTime) / timeSpan, 0.0, 1.0) : 1.0;
            s.output[s.produced++] = static_cast<float>(std::lerp(s.lastOutput, out, mix)
                                                        / fullScalePeakVolts);
        }
        s.lastTime = time;
        s.lastOutput = out;
        if (s.produced == s.block.samples) {
            const Reply reply{s.produced, static_cast<float>(control)};
            if (!write(&reply) || !write(s.output.data(), s.produced) || std::fflush(stdout) != 0)
                std::exit(2);
            s.first += s.block.samples;
            s.previousInput = s.input[s.block.samples - 1];
            s.produced = 0;
            if (!s.receive())
                std::exit(0);
        }
        return 0;
    }
};
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc != 2) return 2;
    std::filesystem::current_path(argv[1]);
    Stream stream;
    if (!read(&stream.start) || stream.start.magic != wireMagic
        || !std::isfinite(stream.start.rate) || stream.start.rate < 8000 || stream.start.rate > 384000
        || !stream.receive())
        return 2;
    ngSpice_Init(Stream::message, nullptr, Stream::exitRequested, Stream::sample,
                 Stream::initialise, nullptr, &stream);
    ngSpice_Init_Sync(Stream::voltage, nullptr, Stream::sync, nullptr, &stream);
    if (command("codemodel ./spice2poly.cm") != 0)
        return 3;
    const auto parameter = [](const char* name, float value) {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << name << '=' << std::setprecision(9) << value;
        return text.str();
    };
    std::ifstream deck("160_stream.cir");
    std::vector<std::string> lines;
    unsigned substitutions = 0;
    for (std::string line; std::getline(deck, line);) {
        if (line.starts_with(".param RATIO=")) {
            for (const auto& [match, replacement] : {
                     std::pair{"RATIO=4", parameter("RATIO", stream.block.ratio)},
                     std::pair{"THRESHV=0.1", parameter("THRESHV", stream.block.thresholdVrms)},
                     std::pair{"MAKEUPDB=0", parameter("MAKEUPDB", stream.block.makeupDb)}}) {
                const auto offset = line.find(match);
                if (offset == std::string::npos) return 3;
                line.replace(offset, std::strlen(match), replacement);
                ++substitutions;
            }
        }
        lines.push_back(std::move(line));
    }
    if (substitutions != 3 || lines.empty()) return 3;
    std::vector<char*> circuit;
    for (auto& line : lines) circuit.push_back(line.data());
    circuit.push_back(nullptr);
    if (ngSpice_Circ(circuit.data()) != 0) return 3;
    command("set num_threads=1");
    // The stream ends on an explicit zero-length block or a closed input pipe.
    // Only the three .save vectors and their time axis are retained by ngspice.
    command("tran 10m 3600 0 2.5u");
    std::fprintf(stderr, "Circuit transient ended before the input stream closed.\n");
    return 3;
}
