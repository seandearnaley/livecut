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
constexpr double pi=std::numbers::pi;

namespace
{
void settle(Resonator& resonator,int count=4800)
{
    for(int i=0;i<count;++i) { float l=0,r=0;resonator.process(l,r); }
}

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

double toneGain(ResonatorParameters p,double frequency,double rate)
{
    Resonator resonator;
    REQUIRE(resonator.prepare(rate));
    REQUIRE(resonator.setParameters(p));
    const auto warm=static_cast<int>(std::ceil(1.5*.001*p.decayMs*rate+.050*rate));
    const auto measure=std::max(128,static_cast<int>(std::lround(4*rate/frequency)));
    double ss=0,cc=0,sc=0,ys=0,yc=0;
    for(int i=0;i<warm+measure;++i)
    {
        const auto phase=2*pi*frequency*i/rate;
        float l=static_cast<float>(.05*std::sin(phase)),r=-l;
        resonator.process(l,r);
        if(i>=warm)
        {
            const auto s=std::sin(phase),c=std::cos(phase);
            ss+=s*s;cc+=c*c;sc+=s*c;ys+=l*s;yc+=l*c;
        }
    }
    const auto determinant=ss*cc-sc*sc;
    return std::hypot((ys*cc-yc*sc)/determinant,(yc*ss-ys*sc)/determinant)/.05;
}
}

TEST_CASE("Disabled Resonator is exact dry, including invalid stored weights","[resonator]")
{
    Resonator resonator;
    REQUIRE(resonator.prepare(48000));
    ResonatorParameters p;
    p.weights={0,0,0,0};
    resonator.setParameters(p);
    for(int i=0;i<1000;++i)
    {
        const auto input=static_cast<float>(std::sin(i*.17));
        float l=input,r=-input;
        resonator.process(l,r);
        REQUIRE(l==input);
        REQUIRE(r==-input);
    }
}

TEST_CASE("Zero weights are explicitly invalid and produce silent resonator wet","[resonator]")
{
    Resonator resonator;
    REQUIRE(resonator.prepare(48000));
    auto p=singleVoice();
    p.weights={0,0,0,0};
    REQUIRE_FALSE(resonator.setParameters(p));
    REQUIRE(resonator.diagnostics().state==Resonator::State::invalidWeights);
    settle(resonator);
    float l=.5f,r=-.5f;
    resonator.process(l,r);
    REQUIRE(l==0);
    REQUIRE(r==0);
}

TEST_CASE("Resonator peak is normalized and selects the requested tone","[resonator]")
{
    const auto p=singleVoice();
    REQUIRE_THAT(toneGain(p,200,48000),WithinAbs(1,.001));
    REQUIRE(toneGain(p,1000,48000)<.05);
}

TEST_CASE("Percussive excitation produces a resonant stereo decay tail","[resonator]")
{
    Resonator resonator;
    REQUIRE(resonator.prepare(48000));
    REQUIRE(resonator.setParameters(singleVoice()));
    settle(resonator);
    double tail=0;
    for(int i=0;i<8000;++i)
    {
        float l=i==0?.8f:0,r=-l;
        resonator.process(l,r);
        if(i>10) tail+=l*l;
        REQUIRE_THAT(r,WithinAbs(-l,1e-9));
    }
    REQUIRE(tail>1e-5);
}

TEST_CASE("Retuning uses a fixed fade and queues only the latest request","[resonator]")
{
    Resonator resonator;
    REQUIRE(resonator.prepare(48000));
    auto p=singleVoice();
    REQUIRE(resonator.setParameters(p));
    settle(resonator);
    p.rootHz=300;
    REQUIRE(resonator.setParameters(p));
    REQUIRE(resonator.diagnostics().retuning);
    p.rootHz=400;
    REQUIRE(resonator.setParameters(p));
    p.rootHz=500;
    REQUIRE(resonator.setParameters(p));
    REQUIRE(resonator.diagnostics().pending);
    settle(resonator,6000);
    REQUIRE_FALSE(resonator.diagnostics().retuning);
    REQUIRE_FALSE(resonator.diagnostics().pending);
    REQUIRE_THAT(resonator.diagnostics().frequencies[0],WithinAbs(500,1e-9));
}

TEST_CASE("Test-only 1k tuning restriction is explicit without frequency clamping","[resonator]")
{
    Resonator resonator;
    REQUIRE(resonator.prepare(1000));
    ResonatorParameters p;
    REQUIRE(resonator.setParameters(p));
    p.rootHz=800;
    p.intervals={24,24,24,24};
    REQUIRE_FALSE(resonator.setParameters(p));
    REQUIRE(resonator.diagnostics().state==Resonator::State::invalidParameters);
}

TEST_CASE("Response peaks and duplicate normalization hold over the advertised rates and tuning extremes","[resonator][numeric]")
{
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.})
        for(float decay:{40.f,5000.f})
            for(double frequency:{20.,55.,110.,200.,800.,3200.})
            {
                auto p=singleVoice();
                p.decayMs=decay;
                p.rootHz=static_cast<float>(frequency);
                int interval=0;
                if(frequency<40) {p.rootHz=40;interval=-12;}
                if(frequency>800) {p.rootHz=800;interval=24;}
                p.intervals={interval,interval,interval,interval};
                p.weights={1,1,1,1};
                const auto delta=std::min(.1*frequency,std::log(1000.)/(2*pi*.001*decay));
                const auto center=toneGain(p,frequency,rate);
                const auto low=toneGain(p,frequency-delta,rate);
                const auto high=toneGain(p,frequency+delta,rate);
                INFO("Fs="<<rate<<" frequency="<<frequency<<" T60="<<decay);
                REQUIRE_THAT(center,WithinAbs(1,.0003));
                REQUIRE(center>=low-.00005);
                REQUIRE(center>=high-.00005);
            }
}

TEST_CASE("Measured waveform pole envelope falls60dB at T60 across all rates","[resonator][numeric]")
{
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.})
        for(float decay:{40.f,5000.f})
        {
            Resonator resonator;
            REQUIRE(resonator.prepare(rate));
            auto p=singleVoice();
            p.rootHz=400;
            p.decayMs=decay;
            REQUIRE(resonator.setParameters(p));
            settle(resonator,static_cast<int>(.06*rate));
            const auto radius=std::exp(-std::log(1000.)/(rate*.001*decay));
            const auto cosine=std::cos(2*pi*400/rate)*2*radius/(1+radius*radius);
            const auto sine=std::sqrt((1-cosine)*(1+cosine));
            const auto first=static_cast<int>(std::lround(.01*rate));
            const auto last=first+static_cast<int>(std::lround(.001*decay*rate));
            double previous=0,initialEnvelope=0,lastEnvelope=0;
            for(int n=0;n<=last;++n)
            {
                float l=n==0?.5f:0,r=-l;
                resonator.process(l,r);
                const auto envelope=std::hypot(l,(cosine*l-radius*previous)/sine);
                if(n==first) initialEnvelope=envelope;
                if(n==last) lastEnvelope=envelope;
                previous=l;
            }
            INFO("Fs="<<rate<<" T60="<<decay);
            REQUIRE(initialEnvelope>0);
            REQUIRE_THAT(lastEnvelope/initialEnvelope,WithinAbs(.001,.000002));
            REQUIRE_THAT(std::pow(resonator.diagnostics().poleRadius,.001*decay*rate),WithinAbs(.001,1e-12));
        }
}

TEST_CASE("Excitation gain and interval controls retain exact explicit meaning","[resonator][numeric]")
{
    for(double rate:{8000.,48000.,384000.})
        for(float db:{-12.f,12.f})
        {
            auto p=singleVoice();p.excitationDb=db;
            REQUIRE_THAT(toneGain(p,200,rate),WithinAbs(std::pow(10.,db/20.),.0004));
        }
    Resonator resonator;
    REQUIRE(resonator.prepare(8000));
    auto p=singleVoice();
    for(int interval=-12;interval<=24;++interval)
    {
        p.intervals={interval,interval,interval,interval};
        REQUIRE(resonator.setParameters(p));
        resonator.reset();
        const auto expected=200*std::exp2(interval/12.);
        REQUIRE_THAT(resonator.diagnostics().frequencies[0],WithinAbs(expected,1e-9));
        REQUIRE(resonator.diagnostics().impulseAbsoluteSumBound<2.0);
    }
}

TEST_CASE("Retune samples are a bounded convex combination of fixed stable banks without allocation","[resonator][allocation]")
{
    for(double rate:{8000.,48000.,384000.})
    {
        Resonator actual,outgoing,incoming;
        REQUIRE(actual.prepare(rate));REQUIRE(outgoing.prepare(rate));REQUIRE(incoming.prepare(rate));
        auto a=singleVoice(),b=a;b.rootHz=500;b.excitationDb=12;
        REQUIRE(actual.setParameters(a));REQUIRE(outgoing.setParameters(a));REQUIRE(incoming.setParameters(b));
        settle(incoming,static_cast<int>(.06*rate));
        for(int n=0;n<static_cast<int>(.1*rate);++n)
        {
            float l=static_cast<float>(.2*std::sin(n*.021)),r=-l,x=l,y=r;
            actual.process(l,r);outgoing.process(x,y);
        }
        const auto frames=static_cast<int>(std::ceil(.05*rate));
        double maximumError=0,boundViolation=0;
        std::size_t allocations=0;
        {
            testing::AllocationCounter counter;
            actual.setParameters(b);
            for(int n=0;n<frames;++n)
            {
                const auto input=static_cast<float>(.2*std::sin((n+117)*.019));
                float l=input,r=-input,old=input,oldR=-input,next=input,nextR=-input;
                actual.process(l,r);outgoing.process(old,oldR);incoming.process(next,nextR);
                const auto fraction=static_cast<double>(n)/(frames-1);
                const auto expected=old+fraction*(static_cast<double>(next)-old);
                maximumError=std::max(maximumError,std::abs(l-expected));
                boundViolation=std::max(boundViolation,std::abs(static_cast<double>(l))-std::max(std::abs(static_cast<double>(old)),std::abs(static_cast<double>(next))));
            }
            allocations=counter.count();
        }
        REQUIRE(allocations==0);
        REQUIRE(maximumError<2e-7);
        REQUIRE(boundViolation<2e-7);
        REQUIRE_FALSE(actual.diagnostics().retuning);
        REQUIRE_THAT(actual.diagnostics().frequencies[0],WithinAbs(500,1e-9));
    }
}

TEST_CASE("Invalid wet retains only the chosen dry proportion and reset preserves latest tuning","[resonator]")
{
    Resonator resonator;REQUIRE(resonator.prepare(48000));
    auto p=singleVoice();p.mix=.25f;p.weights={0,0,0,0};
    REQUIRE_FALSE(resonator.setParameters(p));settle(resonator);
    float l=.5f,r=-.25f;resonator.process(l,r);
    REQUIRE(l==.375f);REQUIRE(r==-.1875f);
    p.weights={1,0,0,0};p.rootHz=300;REQUIRE(resonator.setParameters(p));
    p.rootHz=400;REQUIRE(resonator.setParameters(p));
    p.rootHz=500;REQUIRE(resonator.setParameters(p));
    resonator.reset();
    REQUIRE_FALSE(resonator.diagnostics().retuning);
    REQUIRE_FALSE(resonator.diagnostics().pending);
    REQUIRE_THAT(resonator.diagnostics().frequencies[0],WithinAbs(500,1e-9));
}

TEST_CASE("Extreme weighted banks remain bounded under percussive excitation and queued retunes","[resonator][allocation]")
{
    for(double rate:{8000.,48000.,384000.})
    {
        Resonator resonator;REQUIRE(resonator.prepare(rate));
        ResonatorParameters p;p.enabled=true;p.rootHz=800;p.excitationDb=12;
        p.intervals={-12,0,12,24};p.weights={.01f,.1f,.5f,1};p.decayMs=5000;
        REQUIRE(resonator.setParameters(p));
        double maximum=0,tailEnergy=0;bool finite=true;std::size_t allocations=0;
        {
            testing::AllocationCounter counter;
            for(int n=0;n<static_cast<int>(.25*rate);++n)
            {
                if(n==static_cast<int>(.08*rate)) {p.rootHz=40;p.decayMs=40;resonator.setParameters(p);}
                if(n==static_cast<int>(.09*rate)) {p.rootHz=800;resonator.setParameters(p);}
                if(n==static_cast<int>(.10*rate)) {p.rootHz=110;p.decayMs=500;resonator.setParameters(p);}
                const auto t=static_cast<double>(n)/rate;
                const auto burst=t>=.06 && t<.07;
                float l=burst?static_cast<float>(.8*std::sin(n*.71)*std::exp(-300*(t-.06))):0,r=-l;
                resonator.process(l,r);
                finite &= std::isfinite(l)&&std::isfinite(r);
                maximum=std::max(maximum,std::abs(static_cast<double>(l)));
                if(t>.07 && t<.08) tailEnergy+=l*l;
            }
            allocations=counter.count();
        }
        REQUIRE(allocations==0);
        REQUIRE(finite);
        REQUIRE(maximum<.8*2*std::pow(10.,12./20));
        REQUIRE(tailEnergy>1e-8);
        REQUIRE_FALSE(resonator.diagnostics().pending);
    }
}

TEST_CASE("All muted voices and invalid ranges are reported without a previous-bank substitute","[resonator]")
{
    Resonator resonator;REQUIRE(resonator.prepare(48000));
    auto p=singleVoice();REQUIRE(resonator.setParameters(p));settle(resonator);
    p.weights={0,0,0,0};REQUIRE_FALSE(resonator.setParameters(p));
    float l=1,r=-1;resonator.process(l,r);
    REQUIRE(l==0);REQUIRE(r==0);
    REQUIRE(resonator.diagnostics().state==Resonator::State::invalidWeights);
    p.weights={1,0,0,0};p.rootHz=801;REQUIRE_FALSE(resonator.setParameters(p));
    REQUIRE(resonator.diagnostics().state==Resonator::State::invalidParameters);
    p.rootHz=200;p.intervals[0]=25;REQUIRE_FALSE(resonator.setParameters(p));
    p.intervals[0]=0;p.decayMs=39;REQUIRE_FALSE(resonator.setParameters(p));
    p.decayMs=100;p.excitationDb=13;REQUIRE_FALSE(resonator.setParameters(p));
    REQUIRE_FALSE(resonator.prepare(999));
    REQUIRE(resonator.diagnostics().state==Resonator::State::unsupportedRate);
}

TEST_CASE("Supported-rate preparation preserves explicit invalid settings without rejecting the engine","[resonator][lifecycle]")
{
    for(bool zeroWeights:{false,true})
    {
        INFO("zeroWeights="<<zeroWeights);
        Resonator resonator;REQUIRE(resonator.prepare(48000));
        ResonatorParameters p;p.rootHz=800;p.intervals={24,24,24,24};
        if(zeroWeights) p.weights={0,0,0,0};
        resonator.setParameters(p);
        const auto rate=zeroWeights?48000.:1000.;
        CHECK(resonator.prepare(rate));
        REQUIRE(resonator.diagnostics().state==(zeroWeights?Resonator::State::invalidWeights:Resonator::State::invalidParameters));
        float left=.25f,right=-.5f;resonator.process(left,right);
        REQUIRE(left==.25f);REQUIRE(right==-.5f);
        p.enabled=true;REQUIRE_FALSE(resonator.setParameters(p));settle(resonator);
        left=.25f;right=-.5f;resonator.process(left,right);
        REQUIRE(left==0);REQUIRE(right==0);
        p.weights={1,1,1,1};resonator.setParameters(p);
        REQUIRE(resonator.prepare(8000));
        REQUIRE(resonator.diagnostics().state==Resonator::State::ready);
        REQUIRE(resonator.diagnostics().frequencies[0]==3200);
    }
}

