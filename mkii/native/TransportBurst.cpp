#include "Pipeline.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
using namespace mkii::native;
int main(){
 constexpr int rate=44100,B=512,D=1024,N=8192;
 for(int lead:{0,512,1024,2048,4096}) for(int burst:{1,3,5}) {
  std::vector<float> in(N),ref,actual;
  for(int i=lead;i<N;i++){double t=double(i-lead)/rate;in[i]=float(.8*std::exp(-t/.04)*std::cos(6.283185307*110*t));}
  ref=in; actual=in;
  Pipeline p;
  if(!p.prepare(rate,1,D))return 2;
  float* a=ref.data();if(!p.process(&a,N,{},true))return 3;
  p.stop();if(!p.prepare(rate,1,D))return 4;
  std::this_thread::sleep_for(std::chrono::seconds(5));
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<N;i+=B){
   auto due=start+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(double(std::max(0,i-(burst-1)*B))/rate));
   std::this_thread::sleep_until(due);
   float* x=actual.data()+i;p.process(&x,B,{});
  }
  unsigned dry=0,errors=0;double peak=0;
  for(int i=D;i<N;i++){double e=std::abs(actual[i]-ref[i]);peak=std::max(peak,e);if(e>2e-5){++errors;if(actual[i]==in[i-D])++dry;}}
  std::cout<<"lead="<<lead<<" burst="<<burst<<" missed="<<p.missedSamples()<<" dry="<<dry<<" errors="<<errors<<" peak="<<peak<<" failed="<<p.failedSolves()<<std::endl;
 }
}
