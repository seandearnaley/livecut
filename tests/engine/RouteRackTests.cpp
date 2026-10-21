// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Resonator.h"
#include "engine/RouteRack.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
ResonatorParameters singleVoice()
{
    ResonatorParameters p;
    p.enabled=true;
    p.rootHz=200;
    p.intervals={0,0,0,0};
    p.weights={1,0,0,0};
    p.decayMs=100;
    return p;
}
}

namespace
{
struct ReferenceLane
{
    ImpactEffects impact;
    FrequencyEffects frequency;
    Resonator resonator;
    EffectRoute route=EffectRoute::impactResonatorFrequency;
    void prepare(double rate) {impact.prepare(rate);frequency.prepare(rate);REQUIRE(resonator.prepare(rate));}
    void parameters(const RouteParameters& p)
    {impact.setParameters(p.impact);frequency.setParameters(p.frequency);resonator.setParameters(p.resonator);}
    void process(float& l,float& r,bool selected)
    {
        const auto modulation=[&]
        {frequency.process(l,r,impact.diagnostics().envelope,impact.diagnostics().repeatProgress,selected);};
        if(route==EffectRoute::resonatorImpactFrequency) resonator.process(l,r);
        impact.process(l,r);
        if(route==EffectRoute::impactResonatorFrequency) resonator.process(l,r);
        modulation();
        if(route==EffectRoute::impactFrequencyResonator) resonator.process(l,r);
    }
};

RouteParameters musicalRack()
{
    RouteParameters p;
    p.impact.attackMs=3;p.impact.decayMs=90;
    p.impact.filter.enabled=true;p.impact.filter.cutoffHz=1300;p.impact.filter.chance=.53f;
    p.impact.drive.enabled=true;p.impact.drive.driveDb=13;p.impact.drive.chance=.71f;
    p.impact.transient.enabled=true;p.impact.transient.attackDb=6;p.impact.transient.chance=.37f;
    p.frequency.enabled=true;p.frequency.rateHz=-173;p.frequency.envelopeHz=231;p.frequency.repeatHz=57;
    return p;
}
}

TEST_CASE("Route request waits for a bar and completes a bounded transition","[route]")
{
    RouteRack rack;
    REQUIRE(rack.prepare(48000));
    rack.requestRoute(EffectRoute::resonatorImpactFrequency);
    REQUIRE(rack.diagnostics().active==EffectRoute::impactResonatorFrequency);
    rack.barStarted();
    REQUIRE(rack.diagnostics().transitioning);
    for(int i=0;i<240;++i) {float l=0,r=0;rack.process(l,r);}
    REQUIRE_FALSE(rack.diagnostics().transitioning);
    REQUIRE(rack.diagnostics().active==EffectRoute::resonatorImpactFrequency);
}

TEST_CASE("Fixed rack orders produce distinct nonlinear resonant results","[route]")
{
    std::array<double,3> energy{};
    for(std::size_t route=0;route<3;++route)
    {
        RouteRack rack;
        REQUIRE(rack.prepare(48000));
        RouteParameters p;
        p.impact.drive.enabled=true;
        p.impact.drive.driveDb=18;
        p.resonator=singleVoice();
        p.resonator.excitationDb=12;
        p.frequency.enabled=true;
        p.frequency.rateHz=137;
        rack.setParameters(p);
        rack.requestRoute(static_cast<EffectRoute>(route));
        rack.barStarted();
        rack.startCut({{7,0,0,0,0,1},true});
        for(int i=0;i<24000;++i)
        {
            float l=static_cast<float>(.1*std::sin(i*.027)),r=l;
            rack.process(l,r);
            if(i>12000) energy[route]+=l*l;
        }
    }
    REQUIRE(std::abs(energy[0]-energy[1])>.01);
    REQUIRE(std::abs(energy[0]-energy[2])>.01);
}

TEST_CASE("Disabled Resonator preserves exact legacy audio AD and tagged choices through every route","[route][allocation]")
{
    for(double rate:{1000.,8000.,48000.,384000.})
    {
        RouteRack rack;ImpactEffects impact;FrequencyEffects frequency;
        REQUIRE(rack.prepare(rate));impact.prepare(rate);frequency.prepare(rate);
        auto p=musicalRack();rack.setParameters(p);impact.setParameters(p.impact);frequency.setParameters(p.frequency);
        int firstMismatch=-1;std::size_t allocations=0;bool selected=false;
        {
            testing::AllocationCounter counter;
            for(int n=0;n<16000;++n)
            {
                if(n==1000) rack.requestRoute(EffectRoute::resonatorImpactFrequency);
                if(n==2100) rack.requestRoute(EffectRoute::impactFrequencyResonator);
                if(n==4500) rack.requestRoute(EffectRoute::impactResonatorFrequency);
                if(n==2000 || n==2200 || n==4000 || n==6000) rack.barStarted();
                if(n==3000)
                {
                    p.impact.drive.driveDb=21;p.frequency.rateHz=73;
                    rack.setParameters(p);impact.setParameters(p.impact);frequency.setParameters(p.frequency);
                }
                if(n==9000)
                {
                    rack.reset();impact.reset();frequency.reset();
                    selected=false; // Reset discards the old cut context in both paths.
                    rack.setParameters(p);impact.setParameters(p.impact);frequency.setParameters(p.frequency);
                }
                if(n%173==0)
                {
                    const auto ordinal=static_cast<std::size_t>(n/173);
                    const CutIdentity id{7,static_cast<std::int64_t>(n/5000),0,static_cast<double>(ordinal%4),ordinal%4,4};
                    selected=ordinal%3!=0;
                    rack.startCut({id,selected});impact.startCut(id);
                }
                const auto input=static_cast<float>(.3*std::sin(n*.13)+(n%997<7?.2:0));
                float l=input,r=-input,expected=input,expectedR=-input;
                rack.process(l,r);impact.process(expected,expectedR);
                frequency.process(expected,expectedR,impact.diagnostics().envelope,impact.diagnostics().repeatProgress,selected);
                const auto& a=rack.impact().diagnostics();const auto& b=impact.diagnostics();
                if(l!=expected || r!=expectedR || a.envelope!=b.envelope || a.repeatProgress!=b.repeatProgress
                    || a.filterActive!=b.filterActive || a.driveActive!=b.driveActive || a.transientActive!=b.transientActive)
                    if(firstMismatch<0) firstMismatch=n;
            }
            allocations=counter.count();
        }
        INFO("Fs="<<rate<<" firstMismatch="<<firstMismatch);
        REQUIRE(firstMismatch<0);REQUIRE(allocations==0);
    }
}

TEST_CASE("Every fixed order matches its direct reference chain after the route transition","[route]")
{
    for(auto route:{EffectRoute::impactResonatorFrequency,EffectRoute::resonatorImpactFrequency,EffectRoute::impactFrequencyResonator})
    {
        RouteRack rack;ReferenceLane reference;REQUIRE(rack.prepare(48000));reference.prepare(48000);
        auto p=musicalRack();p.resonator=singleVoice();p.resonator.excitationDb=9;
        rack.setParameters(p);reference.parameters(p);reference.route=route;
        rack.requestRoute(route);rack.barStarted();
        for(int n=0;n<8000;++n)
        {
            if(n%317==0)
            {
                const CutIdentity id{7,0,0,static_cast<double>((n/317)%4),static_cast<std::size_t>((n/317)%4),4};
                rack.startCut({id,true});reference.impact.startCut(id);
            }
            float l=static_cast<float>(.2*std::sin(n*.037)),r=-l,a=l,b=r;
            rack.process(l,r);reference.process(a,b,true);
            if(n>=240) {REQUIRE(l==a);REQUIRE(r==b);}
            REQUIRE(rack.impact().diagnostics().envelope==reference.impact.diagnostics().envelope);
        }
    }
}

TEST_CASE("A route change copies running DSP state and convexly blends while retuning","[route][allocation]")
{
    constexpr double rate=48000;
    RouteRack rack;ReferenceLane outgoing;REQUIRE(rack.prepare(rate));outgoing.prepare(rate);
    auto p=musicalRack();p.resonator=singleVoice();
    rack.setParameters(p);outgoing.parameters(p);
    const RouteCutContext cut{{9,0,0,0,0,1},true};rack.startCut(cut);outgoing.impact.startCut(cut.identity);
    for(int n=0;n<6000;++n)
    {
        float l=static_cast<float>(.2*std::sin(n*.021)),r=l,a=l,b=r;
        rack.process(l,r);outgoing.process(a,b,true);
    }
    auto incoming=outgoing;incoming.route=EffectRoute::resonatorImpactFrequency;
    double maximumError=0;std::size_t allocations=0;
    {
        testing::AllocationCounter counter;
        rack.requestRoute(EffectRoute::resonatorImpactFrequency);rack.barStarted();
        for(int n=0;n<3000;++n)
        {
            if(n==50)
            {
                p.resonator.rootHz=400;
                rack.setParameters(p);outgoing.parameters(p);incoming.parameters(p);
            }
            if(n==100)
            {
                const RouteCutContext next{{9,0,0,1,1,4},false};
                rack.startCut(next);outgoing.impact.startCut(next.identity);incoming.impact.startCut(next.identity);
            }
            const auto selected=n<100;
            float l=static_cast<float>(.2*std::sin((n+17)*.031)),r=-l,a=l,b=r,c=l,d=r;
            rack.process(l,r);outgoing.process(a,b,selected);incoming.process(c,d,selected);
            const auto fraction=std::min(1.0,static_cast<double>(n)/239);
            const auto expected=a+fraction*(static_cast<double>(c)-a);
            maximumError=std::max(maximumError,std::abs(l-expected));
        }
        allocations=counter.count();
    }
    REQUIRE(allocations==0);REQUIRE(maximumError<2e-7);
    REQUIRE(rack.diagnostics().cutsStarted==2);
    REQUIRE(rack.diagnostics().active==EffectRoute::resonatorImpactFrequency);
    REQUIRE_FALSE(rack.resonator().diagnostics().retuning);
}

TEST_CASE("Rapid bar requests finish the current route fade and admit only the latest at a later bar","[route]")
{
    RouteRack rack;REQUIRE(rack.prepare(8000));
    rack.requestRoute(EffectRoute::resonatorImpactFrequency);rack.barStarted();
    for(int n=0;n<40;++n)
    {
        rack.requestRoute(n<20?EffectRoute::impactFrequencyResonator:EffectRoute::impactResonatorFrequency);
        rack.barStarted();float l=0,r=0;rack.process(l,r);
    }
    REQUIRE_FALSE(rack.diagnostics().transitioning);
    REQUIRE(rack.diagnostics().active==EffectRoute::resonatorImpactFrequency);
    for(int n=0;n<100;++n) {float l=0,r=0;rack.process(l,r);}
    REQUIRE(rack.diagnostics().active==EffectRoute::resonatorImpactFrequency);
    rack.barStarted();REQUIRE(rack.diagnostics().transitioning);
    for(int n=0;n<40;++n) {float l=0,r=0;rack.process(l,r);}
    REQUIRE(rack.diagnostics().active==EffectRoute::impactResonatorFrequency);
}
