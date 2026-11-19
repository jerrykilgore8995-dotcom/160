#include "Client.hpp"
#include "../Wire.hpp"
#include <array>
#include <vector>
#include <complex>
#include <cmath>
#include <numbers>
#include <iostream>
#include <iomanip>
#include <algorithm>
int main(){
 using namespace mkii::native;
 constexpr int rate=44100,warm=rate*2,count=rate,hz=10000;
 wchar_t path[32768]{};
 if(!GetFullPathNameW(L"staging/runtime/160-native.dll",32768,path,nullptr))return 2;
 Client p;if(!p.open(path))return 3;
 std::cout<<std::fixed<<std::setprecision(9)<<"input_dBu,ratio,threshold,GR_dB,K2_dBc,nonfundamental_nonK2_dBc,total_residual_dBc";
 for(int k=3;k<=20;++k)std::cout<<",folded_K"<<k<<"_dBc";
 std::cout<<",failed_solves"<<std::endl;
 bool pass=true;
 for(int dbu:{4,8,12,16}) {
  const double amp=.775*std::sqrt(2.)*std::pow(10.,dbu/20.)/mkii::fullScalePeakVolts;
  if(!p.prepare(rate,1,512))return 4;
  int delay=p.latency(),total=warm+count+delay;std::vector<float>x(total);
  for(int i=0;i<total;++i)x[i]=float(amp*std::sin(2*std::numbers::pi*hz*i/rate)*std::min(1.,i/1300.));
  Controls ctl{.2f,4.f,0.f};double gr=0;
  for(int i=0;i<total;i+=512){float* ptr=x.data()+i;if(!p.process(&ptr,std::min(512,total-i),ctl,true))return 5;if(i>=warm+delay)gr=std::max(gr,double(p.readMeters().gainReductionDb));}
  std::array<std::complex<double>,20> bins{};double energy=0,mean=0;
  for(int i=0;i<count;++i){double y=x[warm+delay+i];energy+=y*y;mean+=y;auto z=std::polar(1.,-2*std::numbers::pi*hz*i/rate),v=z;for(auto& b:bins){b+=y*v;v*=z;}}
  double fundamental=std::norm(bins[0]),k2=std::norm(bins[1]),scale=2./(double(count)*count);
  double residual=std::max(0.,energy/count-std::pow(mean/count,2)-scale*fundamental);
  auto db=[](double x){return 10*std::log10(std::max(x,1e-300));};
  std::cout<<dbu<<",4,0.2,"<<gr<<","<<db(k2/fundamental)<<","<<db(std::max(0.,residual-scale*k2)/(scale*fundamental))<<","<<db(residual/(scale*fundamental));
  for(int k=2;k<20;++k)std::cout<<","<<db(std::norm(bins[k])/fundamental);
  auto failures=p.failedSolves();std::cout<<","<<failures<<std::endl;pass=pass&&!failures&&fundamental>0&&std::isfinite(energy);p.stop();
 }
 return pass?0:6;
}
