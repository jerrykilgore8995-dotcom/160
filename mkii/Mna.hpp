#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <stdexcept>
// Dense reference MNA kernel. Device stamps must provide the full residual
// and analytic Jacobian, including charge history, before this is an audio engine.
namespace mkii {
struct Mna {
 int n; std::vector<double> x, previous, residual, jac, delta;
 explicit Mna(int count):n(count),x(n),previous(n),residual(n),jac(n*n),delta(n){}
 double v(int a) const {return a<0?0:x[a];}
 void add(int a,double z){if(a>=0)residual[a]+=z;}
 void j(int a,int b,double z){if(a>=0&&b>=0)jac[a*n+b]+=z;}
 void branch(int a,int b,double i,double g){add(a,i);add(b,-i);j(a,a,g);j(a,b,-g);j(b,a,-g);j(b,b,g);}
 void resistor(int a,int b,double r){branch(a,b,(v(a)-v(b))/r,1/r);}
 void capacitorBE(int a,int b,double c,double dt){
   auto p=[&](int k){return k<0?0.:previous[k];};
   branch(a,b,c/dt*(v(a)-v(b)-p(a)+p(b)),c/dt);
 }
 void voltage(int a,int b,int k,double e){add(a,x[k]);add(b,-x[k]);j(a,k,1);j(b,k,-1);add(k,v(a)-v(b)-e);j(k,a,1);j(k,b,-1);}
 bool linear(){
   for(int i=0;i<n;++i)delta[i]=-residual[i];
   for(int k=0;k<n;++k){
     int p=k;for(int i=k+1;i<n;++i)if(std::abs(jac[i*n+k])>std::abs(jac[p*n+k]))p=i;
     if(!std::isfinite(jac[p*n+k])||std::abs(jac[p*n+k])<1e-24)return false;
     if(p!=k){for(int j=k;j<n;++j)std::swap(jac[k*n+j],jac[p*n+j]);std::swap(delta[k],delta[p]);}
     for(int i=k+1;i<n;++i){double f=jac[i*n+k]/jac[k*n+k];for(int j=k+1;j<n;++j)jac[i*n+j]-=f*jac[k*n+j];delta[i]-=f*delta[k];}
   }
   for(int i=n-1;i>=0;--i){for(int j=i+1;j<n;++j)delta[i]-=jac[i*n+j]*delta[j];delta[i]/=jac[i*n+i];if(!std::isfinite(delta[i]))return false;}return true;
 }
 template<class Stamp> bool step(Stamp stamp,int iterations=40){
   previous=x;
   for(int it=0;it<iterations;++it){
     std::fill(residual.begin(),residual.end(),0);std::fill(jac.begin(),jac.end(),0);stamp(*this);
     double err=0;for(double r:residual)err=std::max(err,std::abs(r));
     if(err<1e-10)return true;
     if(!linear())break;
     double scale=1;for(double d:delta)if(std::abs(d)>0.5)scale=std::min(scale,0.5/std::abs(d));
     for(int i=0;i<n;++i)x[i]+=scale*delta[i];
   }
   x=previous;return false;
 }
};
}
