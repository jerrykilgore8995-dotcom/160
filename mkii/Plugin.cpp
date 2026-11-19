#include "Wire.hpp"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <limits>
#include "Panel.hpp"
#include "PsuNoise.hpp"
#include <vector>
#ifdef MKII_NATIVE
#include "native/Client.hpp"
#else
#include <windows.h>
#endif

namespace {
using namespace mkii;

#ifndef MKII_NATIVE
class CircuitProcess {
public:
    ~CircuitProcess() { close(); }

    bool start(const juce::File& resources, double rate) {
        close();
        const auto directory = resources.getChildFile("Circuit");
        const auto worker = directory.getChildFile("160-circuit.exe");
        if (!worker.existsAsFile()) return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE workerInput = nullptr, workerOutput = nullptr;
        if (!CreatePipe(&workerInput, &input, &attributes, sizeof(Block) + maxBlock * sizeof(float))
            || !CreatePipe(&output, &workerOutput, &attributes, 0)
            || !SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0)
            || !SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0)) {
            dispose(workerInput);
            dispose(workerOutput);
            close();
            return false;
        }
        auto log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            dispose(workerInput);
            dispose(workerOutput);
            close();
            return false;
        }

        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        auto* list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
        const bool initialised = list && InitializeProcThreadAttributeList(list, 1, 0, &size);
        HANDLE inherited[] = {workerInput, workerOutput, log};
        PROCESS_INFORMATION info{};
        bool started = false;
        if (initialised && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                      inherited, sizeof(inherited), nullptr, nullptr)) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = workerInput;
            startup.StartupInfo.hStdOutput = workerOutput;
            startup.StartupInfo.hStdError = log;
            startup.lpAttributeList = list;
            const std::wstring exe(worker.getFullPathName().toWideCharPointer());
            const std::wstring cwd(directory.getFullPathName().toWideCharPointer());
            const auto arguments = L"\"" + exe + L"\" .";
            auto command = arguments;
            started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(),
                                     &startup.StartupInfo, &info);
        }
        if (initialised) DeleteProcThreadAttributeList(list);
        if (list) HeapFree(GetProcessHeap(), 0, list);
        CloseHandle(log);
        dispose(workerInput);
        dispose(workerOutput);
        if (!started) {
            close();
            return false;
        }
        process = info.hProcess;
        CloseHandle(info.hThread);
        const Start hello{wireMagic, 0, rate};
        return send(&hello, sizeof(hello));
    }

    bool submit(const Block& block, const float* data) {
        return send(&block, sizeof(block)) && send(data, block.samples * sizeof(float));
    }

    bool collect(std::uint32_t count, float* data, float& controlVolts) {
        Reply reply;
        if (!read(&reply, sizeof(reply)) || reply.samples != count
            || !read(data, count * sizeof(float))) return false;
        controlVolts = reply.controlVolts;
        return true;
    }

    void close() {
        dispose(input);
        dispose(output);
        if (process && WaitForSingleObject(process, 50) == WAIT_TIMEOUT) TerminateProcess(process, 1);
        dispose(process);
    }

private:
    static void dispose(HANDLE& handle) {
        if (handle) CloseHandle(handle);
        handle = nullptr;
    }

    bool send(const void* source, DWORD bytes) {
        DWORD transferred = 0;
        return WriteFile(input, source, bytes, &transferred, nullptr) && transferred == bytes;
    }

    bool read(void* destination, DWORD bytes) {
        auto* next = static_cast<char*>(destination);
        const auto deadline = GetTickCount64() + 180000;
        while (bytes) {
            DWORD available = 0;
            while (available == 0) {
                if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) return false;
                if (available == 0 && (GetTickCount64() >= deadline
                                      || WaitForSingleObject(process, 1) != WAIT_TIMEOUT)) return false;
            }
            DWORD transferred = 0;
            if (!ReadFile(output, next, std::min(bytes, available), &transferred, nullptr)
                || transferred == 0) return false;
            bytes -= transferred;
            next += transferred;
        }
        return true;
    }

    HANDLE input = nullptr, output = nullptr, process = nullptr;
};

#endif

juce::File circuitResources() {
    // Derive the mounted VST3 module path even when the DAW has another working directory.
    static int moduleAnchor;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&moduleAnchor), &module))
        return {};
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    return juce::File(juce::String(path.data())).getParentDirectory()
        .getParentDirectory().getChildFile("Resources");
}

class Compressor : public juce::AudioProcessor {
public:
    enum Status { live, rendering, failed };
    Compressor()
        : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          state(*this, nullptr, "Settings", parameters()) {
#ifdef MKII_NATIVE
        const auto dll=circuitResources().getChildFile("Native").getChildFile("160-native.dll");
        const auto path=dll.getFullPathName();
        if (engine.open(path.toWideCharPointer())) setLatencySamples(engine.latency());
#endif
    }

    const juce::String getName() const override { return "160 MKII"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.25; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool isBusesLayoutSupported(const BusesLayout& layout) const override {
        const auto in = layout.getMainInputChannelSet();
        return in == layout.getMainOutputChannelSet() &&
               (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }

#ifdef MKII_NATIVE
    void prepareToPlay(double sampleRate,int blockSize) override {
        meterRate=sampleRate;
        inputLevel.store(-60);
        outputLevel.store(-60);
        gr.store(0);
        above.store(false);
        ready=engine.prepare(sampleRate,getTotalNumInputChannels(),blockSize);
        if (ready) setLatencySamples(engine.latency());
        dryLength=std::max(1,getLatencySamples()); dryPosition=0;
        for(auto& channel:dryRing) channel.assign(dryLength,0.f);
        dryScratch.setSize(2,std::max(1,blockSize));
        psu.prepare(sampleRate); mixSmoothed=mix->load()*.01f; noiseSmoothed=noise->load();
        smoothing=float(1-std::exp(-1/(.01*sampleRate)));
        status.store(ready?live:failed);
    }

    void releaseResources() override {
        engine.stop();
        ready=false;
    }
#else
    void prepareToPlay(double sampleRate, int) override {
        resources = circuitResources();
        rate = sampleRate;
        reset();
    }

    void releaseResources() override { reset(); }

    void reset() override {
        for (auto& process : processes) process.close();
        started = false;
        broken = false;
        renderEnded = false;
        status.store(live);
    }

#endif

    void getStateInformation(juce::MemoryBlock& dest) override {
        copyXmlToBinary(*state.copyState().createXml(), dest);
    }

    void setStateInformation(const void* data, int length) override {
        const auto root = getXmlFromBinary(data, length);
        if (root && root->hasTagName(state.state.getType())) state.replaceState(juce::ValueTree::fromXml(*root));
    }

#ifdef MKII_NATIVE
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override {
        processNative(audio,false);
    }

    void processBlockBypassed(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override {
        processNative(audio,true);
    }
#else
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override {
        if (!isNonRealtime() || power->load()<0.5f) {
            renderEnded = true;
            status.store(live);
            gr.store(0);
            return;
        }
        const int channels = getTotalNumInputChannels();
        if (audio.getNumSamples() == 0) return;
        if (renderEnded) reset();
        if (!started && !broken) {
            settings.thresholdVrms = state.getRawParameterValue("threshold")->load();
            settings.ratio = state.getRawParameterValue("ratio")->load();
            settings.makeupDb = state.getRawParameterValue("output")->load();
            for (int c = 0; c < channels; ++c) {
                if (!processes[c].start(resources, rate)) {
                    broken = true;
                    break;
                }
            }
            started = !broken;
        }

        float maxControl = -std::numeric_limits<float>::infinity();
        for (int offset = 0; offset < audio.getNumSamples() && !broken;) {
            settings.samples = static_cast<std::uint32_t>(std::min(audio.getNumSamples() - offset,
                                                                   static_cast<int>(maxBlock)));
            for (int c = 0; c < channels && !broken; ++c)
                broken = !processes[c].submit(settings, audio.getReadPointer(c, offset));
            for (int c = 0; c < channels && !broken; ++c) {
                float volts = 0.0f;
                broken = !processes[c].collect(settings.samples, audio.getWritePointer(c, offset), volts);
                maxControl = std::max(maxControl, volts);
            }
            offset += static_cast<int>(settings.samples);
        }
        if (broken) {
            for (auto& process : processes) process.close();
            audio.clear();
            status.store(failed);
            gr.store(0);
            return;
        }
        status.store(rendering);
        gr.store(std::clamp(maxControl / 0.0061f + settings.makeupDb, 0.0f, 40.0f));
    }

#endif

    juce::AudioProcessorValueTreeState state;
    std::atomic<Status> status{live};
    std::atomic<float> gr{0}, inputLevel{-60}, outputLevel{-60};
    std::atomic<bool> above{false};
    int meterMode=2;

private:
#ifdef MKII_NATIVE
    void processNative(juce::AudioBuffer<float>& audio,bool dry) {
        if (audio.getNumSamples()==0) return;
        if (!ready) {
            audio.clear();
            status.store(failed);
            return;
        }
        // VST3 transfers normalized values, which can move a parameter by
        // a float rounding step. Restore the parameter's declared grid.
        const auto quantize=[](float value,double scale) {
            return static_cast<float>(std::round(static_cast<double>(value)*scale)/scale);
        };
        const native::Controls controls{quantize(threshold->load(),1000),
                                        quantize(ratio->load(),100),
                                        quantize(output->load(),10)};
        dry=dry || power->load()<0.5f;
        // Preserve the aligned original signal in a fixed-size ring. Chunk only if
        // the host exceeds the declared maximum block size.
        bool okay=true;
        const int count=audio.getNumSamples(),channels=audio.getNumChannels();
        double mixedEnergy[2]={};
        for(int offset=0;offset<count;) {
            const int n=std::min(count-offset,dryScratch.getNumSamples());
            for(int i=0;i<n;++i) {
                for(int c=0;c<channels;++c) {
                    auto& slot=dryRing[c][dryPosition];
                    dryScratch.setSample(c,i,slot);
                    slot=audio.getSample(c,offset+i);
                }
                if(++dryPosition==dryLength)dryPosition=0;
            }
            std::array<float*,2> ptr{};
            for(int c=0;c<channels;++c)ptr[c]=audio.getWritePointer(c,offset);
            okay=engine.process(ptr.data(),n,controls,isNonRealtime(),dry)&&okay;
            const float target=mix->load()*.01f, noiseTarget=noise->load();
            for(int i=0;i<n;++i) {
                mixSmoothed+=smoothing*(target-mixSmoothed);
                noiseSmoothed+=smoothing*(noiseTarget-noiseSmoothed);
                if(std::abs(mixSmoothed-target)<1e-6f)mixSmoothed=target;
                if(std::abs(noiseSmoothed-noiseTarget)<1e-6f)noiseSmoothed=noiseTarget;
                for(int c=0;c<channels;++c) {
                    const float noiseSample=psu.next(c);
                    if(!dry) {
                        const float original=dryScratch.getSample(c,i);
                        const float wet=ptr[c][i];
                        ptr[c][i]=mixSmoothed==1.f?wet:(mixSmoothed==0.f?original:original+mixSmoothed*(wet-original));
                        ptr[c][i]+=mixSmoothed*noiseSmoothed*noiseSample;
                    }
                    mixedEnergy[c]+=double(ptr[c][i])*ptr[c][i];
                }
                psu.advance();
            }
            offset+=n;
        }
        status.store(okay && !engine.failedSolves()?live:failed);
        const auto meters=engine.readMeters();
        gr.store(std::clamp(meters.gainReductionDb,0.0f,40.0f));
        above.store(meters.aboveThreshold!=0);
        const auto toDecibels=[](float meanSquare) {
            constexpr double scale=fullScalePeakVolts/0.775;
            return static_cast<float>(10*std::log10(std::max(1.0e-6,meanSquare*scale*scale)));
        };
        const float coefficient=static_cast<float>(-std::expm1(-audio.getNumSamples()/(0.10*meterRate)));
        const auto follow=[coefficient](std::atomic<float>& level,float target) {
            const auto previous=level.load(std::memory_order_relaxed);
            level.store(previous+coefficient*(target-previous),std::memory_order_relaxed);
        };
        follow(inputLevel,toDecibels(meters.inputMeanSquare));
        follow(outputLevel,toDecibels(float(std::max(mixedEnergy[0],mixedEnergy[1])/count)));
    }
#endif

    static juce::AudioProcessorValueTreeState::ParameterLayout parameters() {
        using Param = juce::AudioParameterFloat;
        juce::AudioProcessorValueTreeState::ParameterLayout values;
        juce::NormalisableRange<float> threshold{0.01f, 3.0f, 0.001f, 0.35f};
        juce::NormalisableRange<float> ratio{1.0f, 120.0f, 0.01f, 0.27f};
#ifdef MKII_NATIVE
        constexpr float initialThreshold=0.2f;
#else
        constexpr float initialThreshold=0.1f;
#endif
        values.add(std::make_unique<Param>(juce::ParameterID{"threshold",1},"Threshold",threshold,initialThreshold));
        values.add(std::make_unique<Param>(juce::ParameterID{"ratio", 1}, "Compression", ratio, 4.0f));
        values.add(std::make_unique<Param>(juce::ParameterID{"output", 1}, "Output Gain",
                                          juce::NormalisableRange<float>{-20, 20, 0.1f}, 0.0f));
        values.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"power",1},"Power",true));
        values.add(std::make_unique<Param>(juce::ParameterID{"mix",1},"Mix",juce::NormalisableRange<float>{0,100,0.1f},100.f));
        values.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"noise",1},"Noise",false));
        return values;
    }

    std::atomic<float>* power=state.getRawParameterValue("power");
#ifdef MKII_NATIVE
    double meterRate=44100;
    std::atomic<float>* mix=state.getRawParameterValue("mix");
    std::atomic<float>* noise=state.getRawParameterValue("noise");
    PsuNoise psu;
    std::array<std::vector<float>,2> dryRing;
    juce::AudioBuffer<float> dryScratch;
    int dryLength=1,dryPosition=0;
    float mixSmoothed=1,noiseSmoothed=0,smoothing=.002f;
    native::Client engine;
    std::atomic<float>* threshold=state.getRawParameterValue("threshold");
    std::atomic<float>* ratio=state.getRawParameterValue("ratio");
    std::atomic<float>* output=state.getRawParameterValue("output");
    bool ready=false;
#else
    std::array<CircuitProcess, 2> processes;
    Block settings;
    juce::File resources;
    double rate = 44100;
    bool started = false, broken = false, renderEnded = true;
#endif
};

juce::AudioProcessorEditor* Compressor::createEditor() { return new mkii::Panel<Compressor>(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Compressor(); }
