#include "PresentationCadence.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
int main(){
    unsigned checks=0;
    for(double base:{10.,20.,30.,36.,40.,44.,48.,60.,75.,120.,144.})
        for(double hz:{60.,75.,120.,144.,165.}){
            PresentationCadence policy;int generated=0;const int frames=10000;
            for(int i=0;i<frames;++i)if(policy.Midpoint(1/base,1/hz,true))++generated;
            double expected=std::fmax(0.,std::fmin(1.,hz/base-1))*frames;
            assert(std::abs(generated-expected)<=1.01);++checks;
        }
    PresentationCadence policy;
    assert(!policy.Midpoint(1./48,1./75,true));++checks;
    assert(!policy.Midpoint(1./48,1./75,false));++checks;
    assert(!policy.Midpoint(1./48,1./75,true));++checks;
    assert(policy.Midpoint(1./48,1./75,true));++checks;
    assert(!policy.Midpoint(0,1./75,true));++checks;
    assert(!policy.Midpoint(std::numeric_limits<double>::quiet_NaN(),1./75,true));++checks;
    assert(!policy.Midpoint(1./48,0,true));++checks;
    assert(policy.Midpoint(1./120,1./75,true,false));++checks;
    assert(!policy.Midpoint(1./120,1./75,false,false));++checks;
    assert(!policy.Midpoint(1./48,1./75,true));++checks;
    std::cout<<"PRESENTATION_CADENCE_PASS checks="<<checks<<"; finite refresh budgets, history cuts, invalid intervals\n";
}
