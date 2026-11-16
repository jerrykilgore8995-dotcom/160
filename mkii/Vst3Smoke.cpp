#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>

#ifdef MKII_NATIVE
#include "native/Pipeline.hpp"
#include "native/PlaybackClock.hpp"

#include <chrono>
#include <numbers>
#include <vector>

bool testNativePlayback(juce::AudioPluginInstance& plugin,int rate,int channels) {
    using mkii::native::Pipeline;
    using Clock=std::chrono::steady_clock;
    constexpr int quantum=128;
    const int frames=(rate*3/quantum)*quantum;
    const auto signal=[rate](int n,int channel) {
        const double t=static_cast<double>(n)/rate, angle=2*std::numbers::pi*t;
        const double envelope=(t>0.28 && t<0.43)?0.8:0.22;
        if (n%quantum==60 && (n/quantum)%41==3) return 0.75f;
        return static_cast<float>(envelope*(0.57*std::sin(77*angle+channel)+0.19*std::sin(593*angle)
                                   +0.11*std::sin(3241*angle)+0.08*std::sin(6771*angle)+0.05*std::sin(12949*angle)));
    };
    std::vector<std::vector<float>> expected(channels);
    for (int c=0;c<channels;++c) {
        mkii::native::Solver reference;
        if (!reference.prepare(rate)) return false;
        expected[c].resize(frames);
        for (int i=0;i<frames;++i) {
            const auto input=signal(i,c);
            expected[c][i]=static_cast<float>(reference.step(input,0.2f,4,0));
        }
        if (reference.failures()!=0) return false;
    }

    plugin.setNonRealtime(false);
    plugin.prepareToPlay(rate,quantum);
    const int latency=plugin.getLatencySamples();
    if (latency!=Pipeline::latency) {
        std::cerr<<"Unexpected VST3 latency: "<<latency<<std::endl;
        return false;
    }
    juce::AudioBuffer<float> audio(channels,quantum);
    juce::MidiBuffer midi;
    double maxError=0,maxCallbackMs=0;
    mkii::native::PlaybackClock clock;
    const auto start=Clock::now();
    for (int offset=0;offset<frames+latency;offset+=quantum) {
        for (int c=0;c<channels;++c)
            for (int i=0;i<quantum;++i)
                audio.setSample(c,i,signal(offset+i,c));
        const auto due=start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(offset)/rate));
        clock.waitUntil(due);
        const auto begin=Clock::now();
        plugin.processBlock(audio,midi);
        maxCallbackMs=std::max(maxCallbackMs,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
        for (int c=0;c<channels;++c) {
            for (int i=0;i<quantum;++i) {
                const int sample=offset+i-latency;
                const auto value=audio.getSample(c,i);
                if (!std::isfinite(value)) return false;
                const double target=sample<0?0:expected[c][sample];
                maxError=std::max(maxError,std::abs(value-target));
            }
        }
    }
    std::cout<<rate<<" Hz live playback with "<<channels<<" channel(s), latency "<<latency
             <<" samples, peak waveform error "<<maxError<<", max callback "<<maxCallbackMs<<" ms"<<std::endl;
    return maxError<0.00002 && maxCallbackMs<quantum*1000.0/rate;
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
            if (!testNativePlayback(*plugin,rate,layout.size())) return 9;
        }
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
    std::cout<<"Windows native VST3 passed live mono and stereo playback\n";
#else
    std::cout << "Windows VST3 offline circuit render passed in mono and stereo\n";
#endif
    return 0;
}
