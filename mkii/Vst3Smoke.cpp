#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>

#ifdef MKII_NATIVE
#include "native/Client.hpp"
#include "native/PlaybackClock.hpp"

#include <chrono>
#include <array>
#include <numbers>
#include <vector>

bool testNativePlayback(juce::AudioPluginInstance& plugin,const juce::String& dllPath,int rate,int channels,int quantum,int seconds) {
    using Clock=std::chrono::steady_clock;
    const int frames=((int(rate*3.5)+quantum-1)/quantum)*quantum;
    const auto signal=[rate](int n,int channel) {
        const int period=rate*2/5;
        if(n>=8*period)return 0.f;
        const double t=double(n%period)/rate;
        if(t>.20)return 0.f;
        const double phase=2*std::numbers::pi*(52*t+2.0*(1-std::exp(-t/0.012)));
        return float(.85*std::exp(-t/.038)*std::sin(phase)+.12*std::exp(-t/.002)*std::sin(2*std::numbers::pi*2200*t));
    };
    mkii::native::Client reference;
    if (!reference.open(dllPath.toWideCharPointer()) || !reference.prepare(rate,channels,quantum))
        return false;
    const int expectedLatency=reference.latency();
    const int testFrames=frames+expectedLatency;
    juce::AudioBuffer<float> input(channels,testFrames);
    for (int c=0;c<channels;++c)
        for (int i=0;i<testFrames;++i)
            input.setSample(c,i,signal(i,c));
    auto expected=input;
    if (!reference.process(expected.getArrayOfWritePointers(),testFrames,{},true) ||
        reference.failedSolves()!=0)
        return false;
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
    std::array<unsigned,8> dryByKick{},errorByKick{}; std::array<double,8> peakError{},peakCallback{};
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
                const int k=(offset+i-latency)/(rate*2/5);
                if(offset+i>=latency && k<8){peakError[k]=std::max(peakError[k],std::abs(value-target));if(std::abs(value-target)>0.00002)++errorByKick[k];}
                const int source=offset+i-latency;
                if (source>=0) {
                    const float dry=input.getSample(c,source);
                    if (value==dry && std::abs(target-dry)>0.00002) {++unexpectedDrySamples;if(k<8)++dryByKick[k];}
                }
            }
        }
    }
    for(int k=0;k<8;++k) std::cout<<"KICK "<<k+1<<" dry_samples "<<dryByKick[k]<<" error_samples "<<errorByKick[k]<<" peak_error "<<peakError[k]<<std::endl;
    std::cout<<rate<<" Hz live playback with "<<channels<<" channel(s), "<<quantum
             <<"-sample host blocks, "<<seconds<<" s test, latency "<<latency
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
        for (const int rate : std::initializer_list<int>{}) {
            plugin->releaseResources();
            playbackPassed=testNativePlayback(*plugin,dll.getFullPathName(),rate,layout.size(),128,3) && playbackPassed;
        }
        plugin->releaseResources();
        playbackPassed=testNativePlayback(*plugin,dll.getFullPathName(),44100,layout.size(),512,12)
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
