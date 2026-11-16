#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <span>

namespace mkii {
namespace panel {
struct Mark { double value; float degrees; const char* label; };

constexpr Mark threshold[] = {
    {0.01,-137,"10mV"}, {0.03,-86,".03"}, {0.1,-30,".1"},
    {0.3,24,".3"}, {1,81,"1"}, {3,135,"3"}
};
constexpr Mark ratio[] = {
    {1,-137,"1"}, {1.5,-105,"1.5"}, {2,-71,"2"}, {3,-34,"3"}, {4,0,"4"},
    {6,34,"6"}, {10,67,"10"}, {20,102,"20"}, {120,137,"\xe2\x88\x9e"}
};
constexpr Mark gain[] = {
    {-20,-136,"-20"}, {-15,-102,""}, {-10,-68,"-10"}, {-5,-34,""}, {0,0,"0"},
    {5,34,""}, {10,68,"+10"}, {15,102,""}, {20,136,"+20"}
};
constexpr std::array<std::span<const Mark>,3> scales{threshold,ratio,gain};
constexpr std::array<float,3> dialX{96,230,365};
constexpr float dialY=116;
inline const auto ink=juce::Colour(0xffeae7d9);

inline juce::Point<float> polar(juce::Point<float> centre,float radius,float degrees) {
    const auto radians=juce::degreesToRadians(degrees);
    return centre+juce::Point<float>(std::sin(radians),-std::cos(radians))*radius;
}

inline float dialAngle(std::span<const Mark> scale,double value) {
    for (std::size_t i=1;i<scale.size();++i) {
        const auto [low,left,unused]=scale[i-1];
        const auto [high,right,alsoUnused]=scale[i];
        if (value<=high) return juce::jmap(static_cast<float>((value-low)/(high-low)),left,right);
    }
    return scale.back().degrees;
}

class HardwareLook final : public juce::LookAndFeel_V4 {
public:
    void drawRotarySlider(juce::Graphics& g,int x,int y,int width,int height,float,
                          float,float,juce::Slider& slider) override {
        const auto index=slider.getComponentID().getIntValue();
        const float angle=dialAngle(scales[static_cast<std::size_t>(index)],slider.getValue());
        const juce::Point<float> centre(x+width*0.5f,y+height*0.5f);
        const auto circle=[centre](float radius) {
            return juce::Rectangle<float>(2*radius,2*radius).withCentre(centre);
        };
        g.setColour(juce::Colour(0xff0c0c0b));
        g.fillEllipse(circle(30).translated(2,3));
        g.setGradientFill({juce::Colour(0xff3d3c38),centre.x,centre.y-30,
                           juce::Colour(0xff090908),centre.x,centre.y+30,false});
        g.fillEllipse(circle(30));
        for (int i=0;i<72;++i) {
            const float rib=angle+5.0f*i;
            g.setColour(juce::Colour(i%2==0?0xff55534c:0xff161614));
            g.drawLine({polar(centre,25.5f,rib),polar(centre,29.0f,rib)},0.9f);
        }
        g.setColour(juce::Colour(0xff171715));
        g.fillEllipse(circle(25.5f));
        juce::ColourGradient cap(juce::Colour(0xff797b76),centre.x-23,centre.y+17,
                                  juce::Colour(0xffaaa99f),centre.x+22,centre.y-18,false);
        cap.addColour(0.34,juce::Colour(0xffe6e4d8));
        cap.addColour(0.58,juce::Colour(0xffb8b8af));
        cap.addColour(0.78,juce::Colour(0xfff3f0df));
        g.setGradientFill(cap);
        g.fillEllipse(circle(24));
        g.setColour(juce::Colour(0xfff5f0db).withAlpha(0.55f));
        g.drawEllipse(circle(23.5f),0.65f);
        g.setColour(juce::Colour(0xff282622));
        g.drawLine({polar(centre,4,angle),polar(centre,22,angle)},1.65f);
    }

    void drawButtonBackground(juce::Graphics& g,juce::Button& button,const juce::Colour&,
                              bool highlighted,bool down) override {
        const auto bounds=button.getLocalBounds().toFloat();
        const bool pressed=button.getToggleState() || down;
        g.setColour(juce::Colour(0xff090908));
        g.fillRect(bounds);
        const auto cap=bounds.reduced(3,2).withTrimmedTop(pressed?5.0f:1.0f);
        g.setGradientFill({juce::Colour(pressed?0xff918e80:0xffdedcd1),0,cap.getY(),
                           juce::Colour(highlighted?0xffebe7d9:0xffa09f95),0,cap.getBottom(),false});
        g.fillRect(cap);
        g.setColour(juce::Colour(0xff5e5b51));
        g.drawRect(cap,0.75f);
    }
    void drawButtonText(juce::Graphics&,juce::TextButton&,bool,bool) override {}
};
}

template<class Processor>
class Panel final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit Panel(Processor& owner) : AudioProcessorEditor(owner), processor(owner) {
        using namespace panel;
        const char* ids[]={"threshold","ratio","output"};
        const char* names[]={"Threshold","Compression","Output Gain"};
        for (std::size_t i=0;i<controls.size();++i) {
            auto& control=controls[i];
            control.setComponentID(juce::String(static_cast<int>(i)));
            control.setName(names[i]);
            control.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            control.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
            control.setPopupDisplayEnabled(true,true,this);
            control.setLookAndFeel(&hardware);
            addAndMakeVisible(control);
            attachments[i]=std::make_unique<SliderAttachment>(processor.state,ids[i],control);
        }
        addButton(power,"Power");
        powerAttachment=std::make_unique<ButtonAttachment>(processor.state,"power",power);
        const char* modes[]={"Input","Output","Gain Change"};
        for (std::size_t i=0;i<selectors.size();++i) {
            auto& selector=selectors[i];
            addButton(selector,modes[i]);
            selector.setRadioGroupId(1);
            selector.setToggleState(static_cast<int>(i)==processor.meterMode,juce::dontSendNotification);
            selector.onClick=[this,i] {processor.meterMode=static_cast<int>(i); repaint();};
        }
        power.onStateChange=[this] {repaint();};
        setSize(690,260);
        startTimerHz(30);
    }

    ~Panel() override {
        for (auto& control:controls) control.setLookAndFeel(nullptr);
        for (auto& selector:selectors) selector.setLookAndFeel(nullptr);
        power.setLookAndFeel(nullptr);
    }

    void resized() override {
        for (std::size_t i=0;i<controls.size();++i)
            controls[i].setBounds(static_cast<int>(panel::dialX[i])-32,84,64,64);
        power.setBounds(71,196,48,22);
        for (std::size_t i=0;i<selectors.size();++i)
            selectors[i].setBounds(199+static_cast<int>(i)*61,196,51,22);
    }

    void paint(juce::Graphics& g) override {
        using namespace panel;
        g.fillAll(juce::Colours::black);
        g.setGradientFill({juce::Colour(0xff282725),0,12,juce::Colour(0xff161514),0,247,false});
        g.fillRect(0,10,getWidth(),240);
        drawRail(g,0);
        drawRail(g,248);

        g.setColour(ink);
        g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
        g.drawText("THRESHOLD",39,24,114,19,juce::Justification::centred);
        g.drawText("COMPRESSION",162,24,136,19,juce::Justification::centred);
        g.drawText("OUTPUT",321,22,88,18,juce::Justification::centred);
        g.drawText("GAIN",321,37,88,18,juce::Justification::centred);
        const bool isOn=power.getToggleState();
        const bool above=processor.above.load();
        g.setFont(juce::Font(juce::FontOptions(10.0f,juce::Font::bold)));
        g.drawText("BELOW",29,46,38,13,juce::Justification::centredRight);
        g.drawText("ABOVE",123,46,39,13,juce::Justification::centredLeft);
        drawLamp(g,76,isOn && !above,juce::Colour(0xffffb326));
        drawLamp(g,114,isOn && above,juce::Colour(0xffff3c29));

        g.setColour(ink);
        g.setFont(juce::Font(juce::FontOptions(12.0f,juce::Font::bold)));
        g.drawText("POWER",60,174,70,16,juce::Justification::centred);
        g.drawText("INPUT",199,179,51,14,juce::Justification::centred);
        g.drawText("OUTPUT",260,179,51,14,juce::Justification::centred);
        g.drawText("GAIN",321,169,51,13,juce::Justification::centred);
        g.drawText("CHANGE",321,181,51,13,juce::Justification::centred);
        g.drawLine(202,219,202,231,1.3f);
        g.drawLine(202,231,252,231,1.3f);
        g.drawLine(320,231,369,231,1.3f);
        g.drawLine(369,231,369,219,1.3f);
        g.drawText("METER",253,224,66,14,juce::Justification::centred);

        for (std::size_t i=0;i<scales.size();++i) {
            const juce::Point<float> centre(dialX[i],dialY);
            for (const auto& mark:scales[i]) {
                g.drawLine({polar(centre,34,mark.degrees),polar(centre,39,mark.degrees)},1.45f);
                const auto location=polar(centre,47,mark.degrees).toInt();
                g.drawText(juce::String::fromUTF8(mark.label),location.x-18,location.y-7,36,14,
                           juce::Justification::centred);
            }
        }
        g.setFont(juce::Font(juce::FontOptions(27.0f,juce::Font::bold)));
        g.drawText("160 MKII",436,174,231,34,juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions(14.0f,juce::Font::bold)));
        g.drawText("COMPRESSOR/LIMITER",436,207,231,22,juce::Justification::centred);
        drawMeter(g,isOn);
        if (processor.status.load()==Processor::failed) {
            g.setColour(juce::Colour(0xffff8b68));
            g.setFont(12);
            g.drawText("Circuit processing missed audio",435,229,232,14,juce::Justification::centred);
        }
    }

private:
    void addButton(juce::TextButton& button,const char* name) {
        button.setButtonText(name);
        button.setName(name);
        button.setClickingTogglesState(true);
        button.setLookAndFeel(&hardware);
        addAndMakeVisible(button);
    }

    void drawRail(juce::Graphics& g,int y) const {
        juce::ColourGradient rail(juce::Colour(0xffaaa89c),0,static_cast<float>(y),
                                   juce::Colour(0xff58574e),0,static_cast<float>(y+12),false);
        rail.addColour(0.18,juce::Colour(0xffe4e1cf));
        rail.addColour(0.48,juce::Colour(0xffb4b2a8));
        rail.addColour(0.65,juce::Colour(0xffa5a296));
        g.setGradientFill(rail);
        g.fillRect(0,y,getWidth(),12);
    }

    static void drawLamp(juce::Graphics& g,float x,bool on,juce::Colour tint) {
        g.setColour(juce::Colour(0xff0d0c0a));
        g.fillEllipse(x-6,46,12,12);
        const auto glass=on?tint:tint.withMultipliedBrightness(0.27f);
        if (on) {
            g.setColour(tint.withAlpha(0.20f));
            g.fillEllipse(x-8,44,16,16);
        }
        g.setGradientFill({glass.brighter(0.5f),x-2,48,glass.darker(0.45f),x+3,55,true});
        g.fillEllipse(x-4.5f,47.5f,9,9);
    }

    void drawMeter(juce::Graphics& g,bool isOn) const {
        using panel::polar;
        const juce::Rectangle<float> frame(432,26,237,141);
        const auto glass=frame.reduced(17,17);
        g.setGradientFill({juce::Colour(0xff393634),frame.getX(),frame.getY(),
                           juce::Colour(0xff0a0908),frame.getX(),frame.getBottom(),false});
        g.fillRect(frame);
        g.setColour(juce::Colour(0xff080706));
        g.drawRect(frame,1.2f);
        const auto top=isOn?juce::Colour(0xffffb276):juce::Colour(0xff777267);
        const auto bottom=isOn?juce::Colour(0xfff4dcc0):juce::Colour(0xffb8ac96);
        g.setGradientFill({top,0,glass.getY(),bottom,0,glass.getBottom(),false});
        g.fillRect(glass);
        g.setColour(juce::Colour(0xff695547));
        g.drawRect(glass,1.0f);

        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(glass.toNearestInt().reduced(1));
        const juce::Point<float> pivot(glass.getCentreX(),glass.getBottom()+43);
        const auto angle=[](float dB) {return juce::jmap(dB,-40.0f,20.0f,-44.0f,44.0f);};
        g.setColour(juce::Colour(0xff655a55));
        g.setFont(12);
        juce::Path arc;
        arc.startNewSubPath(polar(pivot,126,angle(-40)));
        for (int dB=-39;dB<=20;++dB) arc.lineTo(polar(pivot,126,angle(static_cast<float>(dB))));
        g.strokePath(arc,juce::PathStrokeType(0.85f));
        for (int dB=-40;dB<=20;dB+=5) {
            const float a=angle(static_cast<float>(dB));
            g.drawLine({polar(pivot,126,a),polar(pivot,dB%10==0?136.0f:132.0f,a)},0.9f);
            if (dB%10!=0) continue;
            const auto location=polar(pivot,143,a).toInt();
            const auto mark=(dB>0?"+":"")+juce::String(dB);
            const int markX=juce::jlimit(static_cast<int>(glass.getX())+15,
                                        static_cast<int>(glass.getRight())-15,location.x);
            g.drawText(mark,markX-13,location.y-7,26,14,juce::Justification::centred);
        }
        g.setFont(juce::Font(juce::FontOptions(15.0f,juce::Font::bold)));
        g.drawText("DECIBELS",glass.toNearestInt().withTrimmedTop(51).withHeight(20),juce::Justification::centred);
        const auto tip=polar(pivot,130,angle(needleDb));
        g.setColour(juce::Colour(0xff3c302b));
        g.drawLine({pivot,tip},1.4f);
        g.setGradientFill({juce::Colours::white.withAlpha(0.20f),0,glass.getY(),
                           juce::Colours::white.withAlpha(0.0f),0,glass.getY()+30,false});
        g.fillRect(glass.withHeight(30));
    }

    void timerCallback() override {
        const float levels[]={processor.inputLevel.load(),processor.outputLevel.load(),-processor.gr.load()};
        const float value=power.getToggleState()?levels[processor.meterMode]:-40.0f;
        const float error=juce::jlimit(-40.0f,20.0f,value)-needleDb;
        if (std::abs(error)>0.02f) {
            needleDb+=0.28f*error;
            repaint(432,26,237,141);
        }
        repaint(68,44,54,16);
        repaint(435,229,232,14);
    }

    using SliderAttachment=juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment=juce::AudioProcessorValueTreeState::ButtonAttachment;
    Processor& processor;
    panel::HardwareLook hardware;
    std::array<juce::Slider,3> controls;
    std::array<juce::TextButton,3> selectors;
    juce::TextButton power;
    std::array<std::unique_ptr<SliderAttachment>,3> attachments;
    std::unique_ptr<ButtonAttachment> powerAttachment;
    float needleDb=0;
};
}
