#pragma once

#include "Sparse.hpp"
#include "../Wire.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <vector>

namespace mkii::native {

constexpr double thermal = 1.380649e-23 * 300.15 / 1.602176634e-19;
constexpr double gmin = 1e-12;

// Signed terminal voltage and its two partial derivatives.
struct Jet {
    double value, p = 0.0, q = 0.0;
};
inline Jet operator+(Jet a, Jet b) { return {a.value+b.value,a.p+b.p,a.q+b.q}; }
inline Jet operator-(Jet a, Jet b) { return {a.value-b.value,a.p-b.p,a.q-b.q}; }
inline Jet operator*(Jet a, double b) { return {a.value*b,a.p*b,a.q*b}; }
inline Jet operator*(Jet a, Jet b) {
    return {a.value*b.value,a.p*b.value+a.value*b.p,a.q*b.value+a.value*b.q};
}
inline Jet operator/(Jet a, Jet b) {
    const double inv = 1.0/b.value, y = a.value*inv;
    return {y,(a.p-y*b.p)*inv,(a.q-y*b.q)*inv};
}
inline Jet squareRoot(Jet x) {
    const double value = std::sqrt(x.value), slope = 0.5/value;
    return {value,x.p*slope,x.q*slope};
}

// Range reduction keeps the polynomial argument within half a log(2).
inline double junctionExp(double x) {
    const int exponent = static_cast<int>(x * 1.4426950408889634 + (x >= 0 ? 0.5 : -0.5));
    const double r = x - exponent * 0.6931471805599453;
    const double exp = 1+r*(1+r*(0.5+r*(1.0/6+r*(1.0/24+r*(1.0/120+r*(1.0/720+
        r*(1.0/5040+r*(1.0/40320+r*(1.0/362880+r*(1.0/3628800+r*(1.0/39916800)))))))))));
    const auto power = std::bit_cast<double>(static_cast<std::uint64_t>(exponent+1023) << 52);
    return exp * power;
}

inline Jet junction(Jet volts, double emissionVoltage, double saturation) {
    if (saturation == 0) return {0};
    const double x = volts.value/emissionVoltage;
    if (x < -3) {
        const double a = 3.0/(2.718281828459045*x);
        const double cube = a*a*a, slope = saturation*3*cube/volts.value;
        return {-saturation*(1 + cube), slope*volts.p, slope*volts.q};
    }
    // The linear continuation above 1.8 V keeps a rejected Newton
    // iterate finite. It is outside the normal operating region.
    const double limited = std::min(x, 70.0);
    const double exp = junctionExp(limited), slope = saturation*exp/emissionVoltage;
    return {saturation*(exp*(1 + x-limited)-1), slope*volts.p, slope*volts.q};
}

inline Jet capacitance(Jet volts, double zero, double potential, double exponent, double fc) {
    if (zero == 0) return {0};
    const double knee = potential*fc;
    double value, slope;
    if (volts.value < knee) {
        const double x = 1-volts.value/potential;
        const double m = std::pow(x,-exponent);
        value = potential*zero*(1-x*m)/(1-exponent);
        slope = zero*m;
    } else {
        const double f = std::pow(1-fc,-exponent);
        const double offset = volts.value-knee;
        const double curvature = f*exponent/(potential*(1-fc));
        value = potential*zero*(1-(1-fc)*f)/(1-exponent)
                + zero*offset*(f+0.5*offset*curvature);
        slope = zero*(f+offset*curvature);
    }
    return {value,slope*volts.p,slope*volts.q};
}

struct Constant { int kind; double value; };
struct R { int a,b; double conductance; bool ratio; };
struct C { int a,b; double value, q1=0, q2=0, q=0; };
struct I { int a,b; double value; };
struct V { int a,b,k; double value; };
struct Control { int a,b,p,m; double gain; };
struct DModel { double is,n,cjo,vj,m,tt,fc; };
struct QModel {
    double is,bf,nf,vaf,ikf,ise,ne,br,nr,var,ikr,isc,nc,cje,vje,mje,cjc,vjc,mjc,tf,tr,fc;
};
struct JModel { double vto,beta,is,lambda; };
struct D { int a,b; double q1=0,q2=0,q=0,slope=0; std::array<int,4> slots{}; };
struct Q { int c,b,e; double f1=0,f2=0,f=0,r1=0,r2=0,r=0; Jet fq{},rq{}; int sign=1; std::array<int,9> slots{}; };
struct J { int d,g,s; std::array<int,9> slots{}; };

struct Circuit {
    int size=0,nodes=0,out=0,control=0,level=0;
    std::vector<Constant> constants;
    std::vector<R> resistors;
    std::vector<C> capacitors;
    std::vector<I> currents;
    std::vector<V> sources;
    std::vector<Control> controlled;
    std::vector<D> diodes;
    std::vector<Q> bjts;
    std::vector<J> jfets;
    std::vector<double> bias;
};
#include "Deck.hpp"

class Solver {
public:
    Solver() { loadDeck(deck); }
    bool prepare(double rate) {
        x = deck.bias;
        previous = x;
        fixed.resize(deck.constants.size());
        supply(0,0.2,4,0);
        matrix.resize(deck.size);
        // Include the transient structure in the symbolic factorization.
        // The actual initial solve below uses zero integration conductance.
        inverse = 1.5*rate;
        history1 = -2.0*rate;
        history2 = 0.5*rate;
        stampLinear();
        stampNonlinear<true>();
        if (!matrix.prepare()) return false;
        cacheNonlinearSlots();
        prepareLinear();
        accept(true);
        inverse = 1.5*rate;
        history1 = -2.0*rate;
        history2 = 0.5*rate;
        misses = 0;
        lastInput=0;
        return true;
    }

    double step(double input, double threshold, double ratio, double makeup) {
        supply(input,threshold,ratio,makeup);
        std::copy(constantRhs.begin(),constantRhs.end(),sampleRhs.begin());
        for (const auto& d : deck.capacitors) {
            const double charge = history1*d.q1+history2*d.q2;
            if (d.a>=0) sampleRhs[d.a]-=charge;
            if (d.b>=0) sampleRhs[d.b]+=charge;
        }
        std::copy(previous.begin(),previous.end(),x.begin());
        bool solved=newton(12,0.16);
        // Change the source in a few stages if a transient leaves Newton's
        // attraction basin. Keep the BDF2 history fixed until the actual
        // sample is solved so intermediate iterations do not advance time.
        for (const int substeps : {2,4,8}) {
            if (solved) break;
            std::copy(previous.begin(), previous.end(), x.begin());
            solved=true;
            for (int i=1;i<=substeps;++i) {
                supply(std::lerp(lastInput,input,static_cast<double>(i)/substeps),
                       threshold,ratio,makeup);
                if (!newton(16,0.16)) {
                    solved=false;
                    break;
                }
            }
        }
        if (solved) {
            accept(false);
            lastInput=input;
        } else {
            std::copy(previous.begin(), previous.end(), x.begin());
            ++misses;
        }
        return x[deck.out]/fullScalePeakVolts;
    }

    double controlVolts() const { return x[deck.control]; }
    double levelVolts() const { return x[deck.level]; }
    int operations() const { return matrix.operationCount(); }
    unsigned failures() const { return misses; }
    double rawOutput() const { return x[deck.out]; }

private:
    double v(int k) const { return k<0 ? fixed[-k-1] : x[k]; }
    void current(int a,int b,double i) {
        // The linear system solves for the negative residual.
        if (a>=0) matrix.rhs[a]-=i;
        if (b>=0) matrix.rhs[b]+=i;
    }
    void derivatives(int a,int b,int p,int n,double g) {
        matrix.add(a,p,g);
        matrix.add(a,n,-g);
        matrix.add(b,p,-g);
        matrix.add(b,n,g);
    }
    void conduct(int a,int b,double g) {
        current(a,b,(v(a)-v(b))*g);
        derivatives(a,b,a,b,g);
    }
    template<bool sizing>
    void terminal(int node,int c,int b,int e,int sign,Jet j,const int* slots) {
        if (node>=0) matrix.rhs[node]-=sign*j.value;
        if constexpr (sizing) {
            matrix.add(node,c,-j.q);
            matrix.add(node,b,j.p+j.q);
            matrix.add(node,e,-j.p);
        } else {
            matrix.addSlot(slots[0],-j.q);
            matrix.addSlot(slots[1],j.p+j.q);
            matrix.addSlot(slots[2],-j.p);
        }
    }

    void cacheNonlinearSlots() {
        const auto assign=[&](auto& slots,const auto& nodes) {
            int at=0;
            for (int row : nodes) for (int col : nodes) slots[at++]=matrix.slot(row,col);
        };
        for (auto& d : deck.diodes) assign(d.slots,std::array{d.a,d.b});
        for (auto& d : deck.bjts) assign(d.slots,std::array{d.c,d.b,d.e});
        for (auto& d : deck.jfets) assign(d.slots,std::array{d.d,d.g,d.s});
    }

    void supply(double input,double threshold,double ratio,double makeup) {
        const double vin = input*0.5*fullScalePeakVolts;
        // Algebraically reduced from VTH/RMSZERO and the two RCTL feedback resistors.
        if (threshold != lastThreshold) {
            lastThreshold=threshold;
            thresholdVolts=0.026220158-0.0065*3.09+0.0065*20.0*std::log10(threshold/0.7071067811865476);
        }
        if (ratio != lastRatio) {
            lastRatio=ratio;
            ratioG=1/(10000*std::max(1e-6,0.0061/0.0065*(1-1/ratio)/0.9399));
        }
        for (std::size_t i=0;i<deck.constants.size();++i) {
            const auto& s=deck.constants[i];
            switch(s.kind) {
                case 1: fixed[i]=vin; break;
                case 2: fixed[i]=-vin; break;
                case 3: fixed[i]=thresholdVolts; break;
                case 4: fixed[i]=-0.0061*makeup; break;
                default: fixed[i]=s.value; break;
            }
        }
    }

    void stampLinear() {
        for (const auto& d : deck.resistors) conduct(d.a,d.b,d.ratio ? ratioG : d.conductance);
        for (auto& d : deck.capacitors) {
            d.q=(v(d.a)-v(d.b))*d.value;
            current(d.a,d.b,inverse*d.q+history1*d.q1+history2*d.q2);
            derivatives(d.a,d.b,d.a,d.b,inverse*d.value);
        }
        for (const auto& d : deck.currents) current(d.a,d.b,d.value);
        for (const auto& d : deck.sources) {
            current(d.a,d.b,x[d.k]);
            derivatives(d.a,d.b,d.k,-1,1);
            current(d.k,-1,v(d.a)-v(d.b)-d.value);
            derivatives(d.k,-1,d.a,d.b,1);
        }
        for (const auto& d : deck.controlled) {
            current(d.a,d.b,(v(d.p)-v(d.m))*d.gain);
            derivatives(d.a,d.b,d.p,d.m,d.gain);
        }
    }

    void prepareLinear() {
        const int n = deck.size;
        const int columns = n+static_cast<int>(fixed.size());
        std::vector<double> dense(n*columns,0.0);
        constantRhs.assign(n,0);
        sampleRhs.resize(n);
        ratioResistors.clear();
        rowOffsets.assign(n+1,0);
        terms.clear();
        matrix.clear();
        const auto bind = [&](int row,int col,double g) {
            if (row<0) return;
            if (col>=0) matrix.add(row,col,g);
            else if (deck.constants[-col-1].kind == 0) {
                constantRhs[row] -= g*fixed[-col-1];
                return;
            }
            dense[row*columns+(col<0?n-col-1:col)] -= g;
        };
        const auto branch = [&](int a,int b,int p,int m,double g) {
            bind(a,p,g); bind(a,m,-g); bind(b,p,-g); bind(b,m,g);
        };
        const auto dc = [&](int a,int b,double i) {
            if (a>=0) constantRhs[a]-=i;
            if (b>=0) constantRhs[b]+=i;
        };
        for (const auto& d : deck.resistors) {
            if (d.ratio) ratioResistors.push_back(d);
            else branch(d.a,d.b,d.a,d.b,d.conductance);
        }
        for (const auto& d : deck.capacitors) branch(d.a,d.b,d.a,d.b,inverse*d.value);
        for (const auto& d : deck.currents) dc(d.a,d.b,d.value);
        for (const auto& d : deck.sources) {
            branch(d.a,d.b,d.k,-1,1);
            branch(d.k,-1,d.a,d.b,1);
            dc(d.k,-1,-d.value);
        }
        for (const auto& d : deck.controlled) branch(d.a,d.b,d.p,d.m,d.gain);
        matrix.cacheLinearPart();
        for (int row=0;row<n;++row) {
            for (int col=0;col<columns;++col) if (const auto g=dense[row*columns+col];g!=0)
                terms.push_back({col<n?col:n-col-1,g});
            rowOffsets[row+1]=static_cast<int>(terms.size());
        }
    }

    void stampStep() {
        matrix.startIteration(sampleRhs);
        for (int row=0;row<deck.size;++row) {
            double sum=matrix.rhs[row];
            for (int j=rowOffsets[row];j<rowOffsets[row+1];++j)
                sum += terms[j].g*v(terms[j].column);
            matrix.rhs[row]=sum;
        }
        for (const auto& d : ratioResistors) conduct(d.a,d.b,ratioG);
        stampNonlinear<false>();
    }

    template<bool sizing>
    void stampNonlinear() {
        stamp_diodes([&]<DModel m>(int begin,int count) {
        for (int at=begin;at<begin+count;++at) {
            auto& d=deck.diodes[at];
            const Jet voltage{v(d.a)-v(d.b),1,0};
            const Jet i=junction(voltage,m.n*thermal,m.is);
            const Jet q=capacitance(voltage,m.cjo,m.vj,m.m,m.fc)+i*m.tt;
            d.q=q.value;
            d.slope=q.p;
            current(d.a,d.b,i.value+voltage.value*gmin+inverse*d.q+history1*d.q1+history2*d.q2);
            const double g=i.p+gmin+inverse*q.p;
            if constexpr (sizing) derivatives(d.a,d.b,d.a,d.b,g);
            else {
                matrix.addSlot(d.slots[0],g);
                matrix.addSlot(d.slots[1],-g);
                matrix.addSlot(d.slots[2],-g);
                matrix.addSlot(d.slots[3],g);
            }
        }
        });
        stamp_bjts([&]<QModel m,int sign>(int begin,int count) {
        for (int at=begin;at<begin+count;++at) {
            auto& d=deck.bjts[at];
            const Jet be{sign*(v(d.b)-v(d.e)),1,0},bc{sign*(v(d.b)-v(d.c)),0,1};
            const Jet f=junction(be,m.nf*thermal,m.is),r=junction(bc,m.nr*thermal,m.is);
            const Jet inf=be*(m.var ? 1.0/m.var : 0.0)+bc*(m.vaf ? 1.0/m.vaf : 0.0);
            Jet baseCharge=Jet{1}-inf;
            if (m.ikf || m.ikr) {
                const Jet knee=Jet{1}+(f*(m.ikf ? 1.0/m.ikf : 0.0)+r*(m.ikr ? 1.0/m.ikr : 0.0))*4;
                baseCharge=baseCharge/(Jet{0.5}+squareRoot(knee)*0.5);
            }
            const Jet transport=(f-r)*baseCharge;
            Jet qf=capacitance(be,m.cje,m.vje,m.mje,m.fc);
            if (m.tf && be.value>0) qf=qf+f*baseCharge*m.tf;
            const Jet qr=capacitance(bc,m.cjc,m.vjc,m.mjc,m.fc)+r*m.tr;
            d.f=qf.value; d.r=qr.value;
            d.fq=qf; d.rq=qr; d.sign=sign;
            Jet ibf=f*(1/m.bf)+junction(be,m.ne*thermal,m.ise)+be*gmin+qf*inverse;
            Jet ibr=r*(1/m.br)+junction(bc,m.nc*thermal,m.isc)+bc*gmin+qr*inverse;
            ibf.value+=history1*d.f1+history2*d.f2;
            ibr.value+=history1*d.r1+history2*d.r2;
            terminal<sizing>(d.c,d.c,d.b,d.e,sign,transport-ibr,d.slots.data());
            terminal<sizing>(d.b,d.c,d.b,d.e,sign,ibf+ibr,d.slots.data()+3);
            terminal<sizing>(d.e,d.c,d.b,d.e,sign,(transport+ibf)*-1,d.slots.data()+6);
        }
        });
        stamp_jfets([&]<JModel m,int sign>(int begin,int count) {
        for (int at=begin;at<begin+count;++at) {
            const auto& d=deck.jfets[at];
            const Jet gs{sign*(v(d.g)-v(d.s)),1,0},gd{sign*(v(d.g)-v(d.d)),0,1};
            const Jet ds=gs-gd;
            const bool reverse=ds.value<0;
            const Jet u=(reverse ? gd : gs)-Jet{m.vto};
            const Jet vd=ds*(reverse ? -1 : 1);
            const Jet beta=Jet{m.beta}+vd*(m.beta*m.lambda);
            const Jet ids=u.value<=0 ? Jet{0} : beta*(vd.value>=u.value ? u*u : vd*(u*2-vd));
            const Jet id=ids*(reverse ? -1 : 1);
            const Jet igs=junction(gs,thermal,m.is)+gs*gmin;
            const Jet igd=junction(gd,thermal,m.is)+gd*gmin;
            terminal<sizing>(d.d,d.d,d.g,d.s,sign,id-igd,d.slots.data());
            terminal<sizing>(d.g,d.d,d.g,d.s,sign,igs+igd,d.slots.data()+3);
            terminal<sizing>(d.s,d.d,d.g,d.s,sign,(id+igs)*-1,d.slots.data()+6);
        }
        });
    }

    bool newton(int iterations,double limit) {
        for (int iter=0;iter<iterations;++iter) {
            stampStep();
            if (!matrix.solve()) return false;
            double maxNode=0, maxBranch=0;
            for (int i=0;i<deck.nodes;++i) maxNode=std::max(maxNode,std::abs(matrix.x[i]));
            for (int i=deck.nodes;i<deck.size;++i) maxBranch=std::max(maxBranch,std::abs(matrix.x[i]));
            if (maxNode<0.004 && maxBranch<0.000006) {
                const auto delta=[&](int n){return n<0?0:matrix.x[n];};
                for (int i=0;i<deck.size;++i) x[i]+=matrix.x[i];
                for (auto& d : deck.diodes) d.q+=d.slope*(delta(d.a)-delta(d.b));
                for (auto& d : deck.bjts) {
                    const double f=d.sign*(delta(d.b)-delta(d.e));
                    const double r=d.sign*(delta(d.b)-delta(d.c));
                    d.f+=d.fq.p*f+d.fq.q*r;
                    d.r+=d.rq.p*f+d.rq.q*r;
                }
                return true;
            }
            double scale=1;
            // Limit the step across junctions, not harmless movement of
            // high-gain op-amp internal nodes or ideal source currents.
            const auto delta=[&](int n){return n<0?0:matrix.x[n];};
            double junctionDelta=0;
            if (maxNode>0.5*limit) {
            for (const auto& d : deck.diodes)
                junctionDelta=std::max(junctionDelta,std::abs(delta(d.a)-delta(d.b)));
            for (const auto& d : deck.bjts)
                junctionDelta=std::max({junctionDelta,std::abs(delta(d.b)-delta(d.e)),
                                        std::abs(delta(d.b)-delta(d.c))});
            for (const auto& d : deck.jfets)
                junctionDelta=std::max(junctionDelta,std::abs(delta(d.g)-delta(d.s)));
            if (junctionDelta>limit) scale=limit/junctionDelta;
            }
            for (int i=0;i<deck.size;++i) x[i]+=scale*matrix.x[i];
        }
        return false;
    }

    void accept(bool dc) {
        for (auto& d : deck.capacitors) {
            d.q=(v(d.a)-v(d.b))*d.value;
            d.q2=dc?d.q:d.q1;
            d.q1=d.q;
        }
        for (auto& d : deck.diodes) {d.q2=dc?d.q:d.q1;d.q1=d.q;}
        for (auto& d : deck.bjts) {d.f2=dc?d.f:d.f1;d.f1=d.f;d.r2=dc?d.r:d.r1;d.r1=d.r;}
        std::copy(x.begin(), x.end(), previous.begin());
    }

    struct Term { int column; double g; };
    Circuit deck;
    std::vector<R> ratioResistors;
    std::vector<Term> terms;
    std::vector<int> rowOffsets;
    std::vector<double> constantRhs,sampleRhs;
    Sparse matrix;
    std::vector<double> x,previous,fixed;
    double inverse=0,history1=0,history2=0,ratioG=0;
    double lastThreshold=0,lastRatio=0,thresholdVolts=0,lastInput=0;
    unsigned misses=0;
};
}
