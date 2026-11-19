#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
namespace mkii {
struct PsuNoise {
    struct Filter {
        double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
        void prepare(double rate,double hz,bool high) {
            const double w=6.283185307179586*std::min(hz,rate*.49)/rate;
            const double c=std::cos(w),a=std::sin(w)/std::sqrt(2.0),d=1+a;
            b0=(high?1+c:1-c)/(2*d);b1=(high?-2:2)*b0;b2=b0;
            a1=-2*c/d;a2=(1-a)/d;z1=z2=0;
        }
        double step(double x) {const double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
    };
    struct Voice {Filter hp,lp;std::uint32_t seed=1;};
    std::array<Voice,2> voices;
    double phase=0,increment=0,amplitude=0;
    void prepare(double rate) {
        phase=0;increment=6.283185307179586*60/rate;amplitude=std::pow(10.,-90./20)*std::sqrt(4096./6)*std::sqrt(3.);
        for(int c=0;c<2;++c){auto& v=voices[c];v.seed=0x12345678u+0x9e3779b9u*c;v.hp.prepare(rate,40,true);v.lp.prepare(rate,15000,false);}
    }
    float next(int c) {
        auto& v=voices[c];auto& x=v.seed;x^=x<<13;x^=x>>17;x^=x<<5;
        const double white=amplitude*(double(x)/2147483648.-1.);
        return float(v.lp.step(v.hp.step(white))+0.000063095734448*std::sin(phase)+0.0001*std::sin(2*phase));
    }
    void advance(){phase+=increment;if(phase>=6.283185307179586)phase-=6.283185307179586;}
};
}
