#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mkii::native {

// The fixed-step nodal solve attenuates the upper band more than the same
// circuit in continuous time. Calibrate the wet output against the deck's
// small-signal AC response, downstream of the detector and VCA.
class OutputResponse {
public:
    void prepare(double rate) {
        const auto upper=std::lower_bound(calibration.begin()+1,calibration.end()-1,rate,
                                         [](const auto& entry,double fs) {return entry.rate<fs;});
        const auto& low=*(upper-1);
        const auto mix=std::clamp((rate-low.rate)/(upper->rate-low.rate),0.0,1.0);
        b0=std::lerp(low.b0,upper->b0,mix);
        b1=std::lerp(low.b1,upper->b1,mix);
        a1=std::lerp(low.a1,upper->a1,mix);
        a2=std::lerp(low.a2,upper->a2,mix);
        b2=1+a1+a2-b0-b1; // Exact unity at DC.
        z1=z2=0;
    }

    double process(double input) {
        const double output=b0*input+z1;
        z1=b1*input-a1*output+z2;
        z2=b2*input-a2*output;
        return output;
    }

private:
    struct Fit {double rate,b0,b1,a1,a2;};
    // Stable high shelves fitted to measured native error at each rate.
    // Denominator interpolation preserves second-order stability.
    static constexpr std::array<Fit,7> calibration{{
        { 32000,1.20954768, 0.27430985, 0.46442478,0.11502336},
        { 44100,1.28525086, 0.19395574, 0.45897567,0.11432321},
        { 48000,1.30620760, 0.15418215, 0.44499599,0.11883674},
        { 88200,1.36959869,-0.43783471, 0.01303732,0.14979798},
        { 96000,1.37072512,-0.50692476,-0.04523820,0.15354259},
        {176400,1.26767836,-0.94940537,-0.56521406,0.22626229},
        {192000,1.25969711,-0.99103145,-0.61391914,0.23669867},
    }};
    double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
};

}
