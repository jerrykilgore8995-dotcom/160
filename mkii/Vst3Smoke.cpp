#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>

#ifdef MKII_NATIVE
#include "native/Client.hpp"
#include "native/PlaybackClock.hpp"

#include <chrono>
#include <numbers>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

bool captureEditor() {
    std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditorIfNeeded());
    if (!editor) return false;
    const auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true,2.0f);
    auto file=juce::File::getCurrentWorkingDirectory().getChildFile("160-MKII-faceplate.png");
    juce::FileOutputStream output(file);
    return output.openedOk() && juce::PNGImageFormat().writeImageToStream(image,output);
}

bool testMakeupMeter(const juce::String& dllPath) {
    constexpr int rate=44100, quantum=512;
    mkii::native::Client engine;
    if (!engine.open(dllPath.toWideCharPointer()) || !engine.prepare(rate,1,quantum)) return false;
    juce::AudioBuffer<float> audio(1,quantum);
    int position=0;
    const auto measure=[&](float peak,float makeup) {
        bool okay=true;
        for (int block=0;block<24;++block) {
            for (int i=0;i<quantum;++i,++position)
                audio.setSample(0,i,peak*std::sin(2*std::numbers::pi*1000*position/rate));
            okay=engine.process(audio.getArrayOfWritePointers(),quantum,{0.2f,4,makeup},true) && okay;
        }
        const auto meters=engine.readMeters();
        std::cout<<"Meter with input peak "<<peak<<", makeup "<<makeup<<": GR "
                 <<meters.gainReductionDb<<" dB, input power "<<meters.inputMeanSquare
                 <<", output power "<<meters.outputMeanSquare<<", above threshold "
                 <<meters.aboveThreshold<<std::endl;
        return std::pair{okay,meters};
    };
    const auto [idleOkay,idle]=measure(0.004f,0);
    const auto [idleBoostOkay,idleBoost]=measure(0.004f,10);
    const auto [activeOkay,active]=measure(0.06f,0);
    const auto [activeBoostOkay,activeBoost]=measure(0.06f,10);
    return idleOkay && idleBoostOkay && activeOkay && activeBoostOkay
        && idle.gainReductionDb<0.1f && idleBoost.gainReductionDb<0.1f
        && !idle.aboveThreshold && !idleBoost.aboveThreshold
        && active.gainReductionDb>2 && active.aboveThreshold && activeBoost.aboveThreshold
        && std::abs(active.gainReductionDb-activeBoost.gainReductionDb)<0.1f
        && std::abs(10*std::log10(activeBoost.inputMeanSquare/active.inputMeanSquare))<0.2f
        && std::abs(10*std::log10(activeBoost.outputMeanSquare/active.outputMeanSquare)-10)<0.8f;
}

bool testNativePlayback(juce::AudioPluginInstance& plugin,const juce::String& dllPath,int rate,int channels,int quantum,int seconds,bool cymbals=false) {
    using Clock=std::chrono::steady_clock;
    const int frames=(rate*seconds/quantum)*quantum;
    const auto signal=[rate,seconds](int n,int channel) {
        const double t=static_cast<double>(n)/rate, angle=2*std::numbers::pi*t;
        const double phase=seconds>3? t-std::floor(t):t;
        const double envelope=(phase>0.28 && phase<0.43)?0.8:0.22;
        if (n%128==60 && (n/128)%41==3) return 0.75f;
        return static_cast<float>(envelope*(0.57*std::sin(77*angle+channel)+0.19*std::sin(593*angle)
                                   +0.11*std::sin(3241*angle)+0.08*std::sin(6771*angle)+0.05*std::sin(12949*angle)));
    };
    mkii::native::Client reference;
    if (!reference.open(dllPath.toWideCharPointer()) || !reference.prepare(rate,channels,quantum))
        return false;
    const int expectedLatency=reference.latency();
    const int testFrames=frames+expectedLatency;
    juce::AudioBuffer<float> input(channels,testFrames);
    for (int c=0;c<channels;++c) {
        std::uint32_t random=0x7613f042u+static_cast<unsigned>(c);
        double previousNoise=0,olderNoise=0;
        for (int i=0;i<testFrames;++i) {
            if (!cymbals) {
                input.setSample(c,i,signal(i,c));
                continue;
            }
            random^=random<<13;
            random^=random>>17;
            random^=random<<5;
            const double noise=random*(2.0/4294967295.0)-1.0;
            const double high=(noise-2*previousNoise+olderNoise)*0.25;
            olderNoise=previousNoise;
            previousNoise=noise;
            const int burst=i/(rate/4);
            const double age=double(i%(rate/4))/rate;
            const double peak=burst%2==0?0.08:0.25;
            input.setSample(c,i,static_cast<float>(peak*std::exp(-10*age)*high));
        }
    }
    auto expected=input;
    if (!reference.process(expected.getArrayOfWritePointers(),testFrames,{},true) ||
        reference.failedSolves()!=0) {
        std::cerr<<(cymbals?"Cymbal fixture":"Multitone fixture")
                 <<" reference render failed, solver failures "<<reference.failedSolves()<<std::endl;
        return false;
    }
    reference.stop();

    plugin.setNonRealtime(false);
    plugin.prepareToPlay(rate,quantum);
    const int latency=plugin.getLatencySamples();
    if (latency!=expectedLatency) {
        std::cerr<<"Unexpected VST3 latency: "<<latency<<std::endl;
        return false;
    }
    juce::AudioBuffer<float> audio(channels,quantum);
    juce::MidiBuffer midi;
    double maxError=0,maxCallbackMs=0;
    unsigned unexpectedDrySamples=0;
    mkii::native::PlaybackClock clock;
    const auto start=Clock::now();
    for (int offset=0;offset<testFrames;offset+=quantum) {
        const int count=std::min(quantum,testFrames-offset);
        audio.setSize(channels,count,false,false,true);
        for (int c=0;c<channels;++c)
            audio.copyFrom(c,0,input,c,offset,count);
        const auto due=start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(offset)/rate));
        clock.waitUntil(due);
        const auto begin=Clock::now();
        plugin.processBlock(audio,midi);
        maxCallbackMs=std::max(maxCallbackMs,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
        for (int c=0;c<channels;++c) {
            for (int i=0;i<count;++i) {
                const auto value=audio.getSample(c,i);
                if (!std::isfinite(value)) return false;
                const double target=expected.getSample(c,offset+i);
                maxError=std::max(maxError,std::abs(value-target));
                const int source=offset+i-latency;
                if (source>=0) {
                    const float dry=input.getSample(c,source);
                    if (value==dry && std::abs(target-dry)>0.00002) ++unexpectedDrySamples;
                }
            }
        }
    }
    std::cout<<rate<<" Hz live playback with "<<channels<<" channel(s), "<<quantum
             <<"-sample host blocks, "<<seconds<<" s "<<(cymbals?"cymbal-like":"multitone")<<" test, latency "<<latency
             <<" samples, peak waveform error "<<maxError<<", max callback "<<maxCallbackMs
             <<" ms, unexpected dry samples "<<unexpectedDrySamples<<std::endl;
    return unexpectedDrySamples==0 && maxError<0.00002 && maxCallbackMs<quantum*1000.0/rate;
}
#endif

int main(int argc, char** argv) {
    std::cout << "Starting Windows VST3 smoke test" << std::endl;
    if (argc != 2) return 2;
    std::cout << "Module: " << argv[1] << std::endl;
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::AudioPluginFormatManager formats;
    auto format = std::make_unique<juce::VST3PluginFormat>();
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format->findAllTypesForFile(descriptions, juce::String::fromUTF8(argv[1]));
    if (descriptions.size() != 1) {
        std::cerr << "Expected one audio effect, found " << descriptions.size() << std::endl;
        return 3;
    }
    formats.addFormat(format.release());
    juce::String error;
    auto plugin = formats.createPluginInstance(*descriptions[0], 44100, 256, error);
    if (!plugin) {
        std::cerr << error << '\n';
        return 4;
    }
    std::cout << "Loaded VST3 audio effect" << std::endl;
#ifdef MKII_NATIVE
    bool playbackPassed=true;
    const auto dll=juce::File(juce::String::fromUTF8(argv[1])).getChildFile("Contents")
        .getChildFile("Resources").getChildFile("Native").getChildFile("160-native.dll");
    if (!captureEditor()) {
        std::cerr<<"Could not render the faceplate"<<std::endl;
        return 10;
    }
    if (!testMakeupMeter(dll.getFullPathName())) {
        std::cerr<<"The meter did not distinguish makeup gain from compression"<<std::endl;
        return 11;
    }
#endif
    for (const auto layout : {juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo()}) {
        std::cout << "Configuring " << layout.size() << " channel(s)" << std::endl;
        plugin->releaseResources();
        juce::AudioProcessor::BusesLayout channels;
        channels.inputBuses.add(layout);
        channels.outputBuses.add(layout);
        if (!plugin->setBusesLayout(channels)) {
            std::cerr << "The VST3 rejected the bus arrangement" << std::endl;
            return 5;
        }
#ifdef MKII_NATIVE
        for (const int rate : {44100,48000}) {
            plugin->releaseResources();
            playbackPassed=testNativePlayback(*plugin,dll.getFullPathName(),rate,layout.size(),128,3) && playbackPassed;
        }
        plugin->releaseResources();
        playbackPassed=testNativePlayback(*plugin,dll.getFullPathName(),44100,layout.size(),512,12)
                       && playbackPassed;
        plugin->releaseResources();
        playbackPassed=testNativePlayback(*plugin,dll.getFullPathName(),44100,layout.size(),512,6,true)
                       && playbackPassed;
#else
        plugin->setNonRealtime(false);
        plugin->prepareToPlay(44100, 256);
        juce::AudioBuffer<float> audio(layout.size(), 256);
        juce::MidiBuffer midi;
        constexpr float inputPeaks[] = {0.1f, 0.025f};
        // The same 256 samples streamed directly into the reference worker,
        // with the default 0.1 V threshold and 4:1 ratio.
        constexpr float circuitPeaks[] = {0.004445671f, 0.002414901f};
        for (int c = 0; c < audio.getNumChannels(); ++c)
            for (int n = 0; n < audio.getNumSamples(); ++n)
                audio.setSample(c, n, inputPeaks[c] * std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * n / 44100));
        auto dry = audio;
        plugin->processBlock(audio, midi);
        if (std::abs(audio.getSample(0, 40) - dry.getSample(0, 40)) > 1.0e-7f) {
            std::cerr << "Unexpected change during live playback" << std::endl;
            return 6;
        }
        plugin->releaseResources();
        std::cout << "Rendering " << layout.size() << " channel(s) offline" << std::endl;
        plugin->setNonRealtime(true);
        plugin->prepareToPlay(44100, 256);
        plugin->processBlock(audio, midi);
        for (int c = 0; c < audio.getNumChannels(); ++c) {
            float difference = 0, peak = 0;
            for (int n = 0; n < audio.getNumSamples(); ++n) {
                const float value = audio.getSample(c, n);
                if (!std::isfinite(value)) return 7;
                peak = std::max(peak, std::abs(value));
                difference = std::max(difference, std::abs(value - dry.getSample(c, n)));
            }
            std::cout << "Channel " << c << " peak " << peak << ", change " << difference << std::endl;
            if (std::abs(peak - circuitPeaks[c]) > 0.00004f || difference < 0.005f) {
                std::cerr << "Expected circuit peak " << circuitPeaks[c] << std::endl;
                return 8;
            }
        }
#endif
    }
    plugin->releaseResources();
#ifdef MKII_NATIVE
    if (!playbackPassed) return 9;
    std::cout<<"Windows native VST3 passed live mono and stereo playback\n";
#else
    std::cout << "Windows VST3 offline circuit render passed in mono and stereo\n";
#endif
    return 0;
}
