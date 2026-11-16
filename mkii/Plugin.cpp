#include "Wire.hpp"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#ifdef MKII_NATIVE
#include "native/Pipeline.hpp"
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

#endif

class Compressor : public juce::AudioProcessor {
public:
    enum Status { live, rendering, failed };
    Compressor()
        : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          state(*this, nullptr, "Settings", parameters()) {}

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
    void prepareToPlay(double sampleRate, int) override {
        setLatencySamples(native::Pipeline::latency);
        ready=engine.prepare(sampleRate,getTotalNumInputChannels());
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
        if (!isNonRealtime()) {
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

        float maxControl = 0.0f;
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
    std::atomic<float> gr{0};

private:
#ifdef MKII_NATIVE
    void processNative(juce::AudioBuffer<float>& audio,bool dry) {
        if (audio.getNumSamples()==0) return;
        if (!ready) {
            audio.clear();
            status.store(failed);
            return;
        }
        const native::Controls controls{threshold->load(),ratio->load(),output->load()};
        const auto okay=engine.process(audio.getArrayOfWritePointers(),audio.getNumSamples(),
                                       controls,isNonRealtime(),dry);
        status.store(okay && !engine.failedSolves()?live:failed);
        gr.store(std::clamp(engine.controlVolts()/0.0061f+controls.makeup,0.0f,40.0f));
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
        return values;
    }

#ifdef MKII_NATIVE
    native::Pipeline engine;
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

class Panel : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit Panel(Compressor& owner) : AudioProcessorEditor(owner), processor(owner) {
        const char* ids[] = {"threshold", "ratio", "output"};
        const char* labels[] = {"Threshold (V RMS)", "Compression", "Output Gain (dB)"};
        for (int i = 0; i < 3; ++i) {
            controls[i].setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            controls[i].setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 21);
            controls[i].setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffcec3a6));
            controls[i].setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff43423f));
            controls[i].setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xfff0ead9));
            title[i].setText(labels[i], juce::dontSendNotification);
            title[i].setJustificationType(juce::Justification::centred);
            title[i].setColour(juce::Label::textColourId, juce::Colour(0xffe0d9c7));
            addAndMakeVisible(controls[i]);
            addAndMakeVisible(title[i]);
            attachments[i] = std::make_unique<Attachment>(owner.state, ids[i], controls[i]);
        }
        setSize(568, 198);
        startTimerHz(15);
    }

    void resized() override {
        for (int i = 0; i < 3; ++i) {
            title[i].setBounds(248 + i * 101, 37, 98, 22);
            controls[i].setBounds(248 + i * 101, 60, 98, 95);
        }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff181916));
        g.setColour(juce::Colour(0xffdcd3bc));
        g.setFont(22);
        g.drawText("160 MKII", 20, 12, 155, 31, juce::Justification::centredLeft);
        g.setFont(13);
        g.drawText("0.1", 123, 13, 44, 31, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff30302c));
        g.fillRoundedRectangle(20, 53, 210, 94, 4);
        g.setColour(juce::Colour(0xfff3dfaa));
        g.fillRoundedRectangle(28, 60, 194, 80, 2);
        g.setColour(juce::Colour(0xff242018));
        g.setFont(12);
        g.drawText("GAIN REDUCTION", 52, 113, 146, 18, juce::Justification::centred);
        const auto origin = juce::Point<float>(125, 158);
        for (int i = 0; i <= 4; ++i) {
            const float angle = -0.75f + 0.375f * static_cast<float>(i);
            const juce::Point<float> radial{std::sin(angle), -std::cos(angle)};
            const auto tick = origin + radial * 84.0f;
            g.drawLine({tick, origin + radial * 76.0f});
            g.drawText(juce::String(40 - 10 * i), static_cast<int>(tick.x) - 11,
                       static_cast<int>(tick.y) - 16, 22, 14, juce::Justification::centred);
        }
        const float angle = 0.75f - 1.5f * processor.gr.load() / 40.0f;
        g.setColour(juce::Colour(0xff862620));
        g.drawLine({origin + juce::Point<float>{std::sin(angle), -std::cos(angle)} * 23.0f,
                    origin + juce::Point<float>{std::sin(angle), -std::cos(angle)} * 74.0f}, 1.5f);
        g.setColour(juce::Colour(0xffbdb7a7));
        g.setFont(13);
        const auto working = processor.status.load();
#ifdef MKII_NATIVE
        g.drawText(working==Compressor::failed?"Circuit processing missed audio":"Native circuit processing",
                   20,162,528,24,juce::Justification::centredLeft);
#else
        g.drawText(working == Compressor::failed ? "Circuit render failed. Restart the offline bounce."
                   : working == Compressor::rendering ? "Rendering through the SPICE circuit"
                   : "Offline render only. Live playback passes through.",
                   20, 162, 528, 24, juce::Justification::centredLeft);
#endif
    }

private:
    void timerCallback() override { repaint(); }
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    Compressor& processor;
    std::array<juce::Slider, 3> controls;
    std::array<juce::Label, 3> title;
    std::array<std::unique_ptr<Attachment>, 3> attachments;
};

juce::AudioProcessorEditor* Compressor::createEditor() { return new Panel(*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Compressor(); }
