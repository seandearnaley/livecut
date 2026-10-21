// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Cutter.h"
#include "engine/AllocationCounter.h"
#include "legacy/LegacyCutter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>

using namespace livecut::engine;

namespace
{
constexpr int rate=8000;
CutParameters commonParameters()
{
    CutParameters p;
    p.minPhraseBars=p.maxPhraseBars=1;
    p.minRepeats=p.maxRepeats=0;
    p.minPan=p.maxPan=0;
    p.minCents=p.maxCents=0;
    p.stutterChance=0;
    return p;
}
void configure(Cutter& cutter,Procedure procedure=Procedure::cutProc11)
{
    cutter.prepare(rate);cutter.setSeed(7);cutter.setParameters(commonParameters());cutter.setProcedure(procedure);
}
StereoFrame input(int frame)
{
    constexpr auto pi=3.14159265358979323846;
    return {static_cast<float>(.2*std::sin(2*pi*110*frame/rate)+.1*std::sin(2*pi*500*frame/rate)),
            static_cast<float>(.25*std::cos(2*pi*200*frame/rate))};
}
Position position(int frame)
{
    const auto beats=frame/4000.0;
    const auto bar=static_cast<std::int64_t>(std::floor(beats/4));
    return {bar,beats-static_cast<double>(bar)*4};
}
StereoFrame tick(Cutter& cutter,int frame)
{
    auto raw=input(frame);
    cutter.process(&raw.left,&raw.right,1,{120,4},position(frame));
    return raw;
}
struct Bounds final:Cutter::SourceContext
{
    std::optional<Cutter::TransportBounds> sourceBoundsAt(std::int64_t frame) const override
    {
        if(frame<0) return std::nullopt;
        return Cutter::TransportBounds{71,0,std::nullopt};
    }
};
}

TEST_CASE("Cutter routes its processed source through the selected Resonator","[resonance][integration]")
{
    Cutter dry,effected;configure(dry);configure(effected);
    ResonatorParameters p;p.enabled=true;p.intervals={0,0,0,0};effected.setResonator(p);
    double difference=0;
    for(int n=0;n<8000;++n)
    {
        const auto a=tick(dry,n),b=tick(effected,n);
        difference+=std::abs(a.left-b.left)+std::abs(a.right-b.right);
    }
    REQUIRE(difference>100);
    REQUIRE(effected.resonator().diagnostics().state==Resonator::State::ready);
}

TEST_CASE("Cutter does not substitute source audio for an invalid selected Resonator","[resonance][integration]")
{
    Cutter cutter;configure(cutter);
    ResonatorParameters p;p.enabled=true;p.weights={0,0,0,0};cutter.setResonator(p);
    double energy=0;
    for(int n=0;n<4000;++n)
    {
        const auto output=tick(cutter,n);
        if(n>100) energy+=output.left*output.left+output.right*output.right;
    }
    REQUIRE(energy==0);
    REQUIRE(cutter.resonator().diagnostics().state==Resonator::State::invalidWeights);
}

TEST_CASE("Cutter's three orders produce different nonlinear resonant output","[resonance][integration]")
{
    std::array<double,3> energy{};
    for(std::size_t order=0;order<3;++order)
    {
        Cutter cutter;configure(cutter);
        ResonatorParameters p;p.enabled=true;p.rootHz=200;p.excitationDb=12;p.intervals={0,0,0,0};cutter.setResonator(p);
        ImpactParameters impact;impact.drive.enabled=true;impact.drive.driveDb=18;cutter.setImpact(impact);
        FrequencyParameters frequency;frequency.enabled=true;frequency.rateHz=73;cutter.setFrequency(frequency);
        cutter.setEffectRoute(static_cast<EffectRoute>(order));
        for(int n=0;n<8000;++n)
        {
            const auto output=tick(cutter,n);
            if(n>4000) energy[order]+=output.left*output.left+output.right*output.right;
        }
    }
    REQUIRE(std::abs(energy[0]-energy[1])>1);
    REQUIRE(std::abs(energy[0]-energy[2])>1);
}

TEST_CASE("Disabled Resonance retains exact legacy output including Crusher and Comb","[resonance][integration][legacy]")
{
    const auto procedure=GENERATE(Procedure::cutProc11,Procedure::warpCut,Procedure::sqPusher,Procedure::bounce);
    Cutter current;configure(current,procedure);
    legacy::Cutter old;old.prepare(rate);old.setSeed(7);old.setProcedure(static_cast<legacy::Procedure>(procedure));old.setParameters(commonParameters());
    const CrusherParameters crusher{true,6,14,1700,6000};
    const CombParameters comb{true,CombParameters::Type::feedBack,.4f,2,17};
    current.setCrusher(crusher);old.setCrusher(crusher);current.setComb(comb);old.setComb(comb);
    ResonatorParameters p;p.rootHz=800;p.decayMs=5000;p.excitationDb=12;p.intervals={24,24,24,24};current.setResonator(p);
    current.setEffectRoute(EffectRoute::impactFrequencyResonator);
    bool identical=true;
    for(int n=0;n<24000;++n)
    {
        auto expected=input(n);const auto pos=position(n);
        old.process(&expected.left,&expected.right,1,{120,4},{pos.bar,pos.beatsIntoBar});
        const auto actual=tick(current,n);
        identical &= actual.left==expected.left && actual.right==expected.right;
    }
    REQUIRE(identical);
}

TEST_CASE("Resonator parameters freeze with blocks while route changes wait for a bar","[resonance][integration][snapshot]")
{
    Cutter cutter;configure(cutter);OnsetDetector detector;Bounds bounds;cutter.setTransformSource(detector,bounds);
    FractureParameters fracture;fracture.procedure=FractureProcedure::grainCut;fracture.activity=0;cutter.setFracture(fracture);
    ResonatorParameters p;p.enabled=true;p.intervals={0,0,0,0};cutter.setResonator(p);
    for(int n=0;n<16040;++n)
    {
        if(n==1000) {p.rootHz=200;cutter.setResonator(p);cutter.setEffectRoute(EffectRoute::impactFrequencyResonator);}
        tick(cutter,n);
        if(n==1000 || n==3999)
        {
            REQUIRE_FALSE(cutter.resonator().diagnostics().retuning);
            REQUIRE(cutter.resonator().diagnostics().frequencies[0]==110);
        }
        if(n==4000) REQUIRE(cutter.resonator().diagnostics().retuning);
        if(n==4399) REQUIRE(cutter.resonator().diagnostics().frequencies[0]==200);
        if(n==15999) REQUIRE(cutter.routeDiagnostics().active==EffectRoute::impactResonatorFrequency);
        if(n==16000) REQUIRE(cutter.routeDiagnostics().transitioning);
    }
    REQUIRE(cutter.routeDiagnostics().active==EffectRoute::impactFrequencyResonator);
    REQUIRE_FALSE(cutter.routeDiagnostics().transitioning);
}

TEST_CASE("Resonance routing and retuning allocate nothing in any source family","[resonance][integration][allocation]")
{
    for(auto family:{FractureProcedure::classic,FractureProcedure::grainCut,FractureProcedure::tapeCut})
    {
        Cutter cutter;configure(cutter);OnsetDetector detector;Bounds bounds;cutter.setTransformSource(detector,bounds);
        FractureParameters f;f.procedure=family;cutter.setFracture(f);
        ResonatorParameters p;p.enabled=true;cutter.setResonator(p);
        std::size_t allocations=0;
        {
            testing::AllocationCounter counter;
            for(int n=0;n<24000;++n)
            {
                if(n%1000==0) {p.rootHz=n%2000==0?110.f:220.f;cutter.setResonator(p);}
                if(n==1000) cutter.setEffectRoute(EffectRoute::resonatorImpactFrequency);
                tick(cutter,n);
            }
            allocations=counter.count();
        }
        REQUIRE(allocations==0);
        REQUIRE(cutter.error()==Cutter::Error::none);
    }
}

TEST_CASE("Cutter reprepare keeps an invalid disabled Resonator local to its wet path","[resonance][integration][lifecycle]")
{
    Cutter cutter;configure(cutter);
    ResonatorParameters p;p.weights={0,0,0,0};cutter.setResonator(p);
    tick(cutter,0);
    REQUIRE(cutter.resonator().diagnostics().state==Resonator::State::invalidWeights);
    REQUIRE_NOTHROW(cutter.prepare(rate));
    tick(cutter,0);
    REQUIRE(cutter.error()==Cutter::Error::none);
    REQUIRE(cutter.resonator().diagnostics().state==Resonator::State::invalidWeights);
}
