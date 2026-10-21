// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Cutter.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <algorithm>
#include <vector>
#include <stdexcept>

using namespace livecut::engine;

namespace
{
constexpr double rate = 8000.0;

struct Bounds final : Cutter::SourceContext
{
    std::optional<Cutter::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
    {
        if (frame < 0)
            return std::nullopt;
        return Cutter::TransportBounds { 71, 0, std::nullopt };
    }
};

void configure (Cutter& cutter)
{
    cutter.prepare (rate);
    CutParameters p;
    p.minPhraseBars = p.maxPhraseBars = 2;
    p.minPan = p.maxPan = 0;
    p.minCents = p.maxCents = 0;
    p.stutterChance = 0;
    cutter.setParameters (p);
}

StereoFrame tick (Cutter& cutter, int frame)
{
    auto l = .5f, r = .25f;
    const auto beat = frame / 4000.0;
    const auto bar = static_cast<std::int64_t> (beat / 4.0);
    cutter.process (&l, &r, 1, { 120,4 }, {bar,beat-bar*4.0});
    return {l,r};
}
}

TEST_CASE ("Tape zero endpoints stop instead of passing held source DC", "[fracture][integration]")
{
    Cutter cutter;
    configure (cutter);
    OnsetDetector detector;
    Bounds bounds;
    cutter.setTransformSource (detector,bounds);
    FractureParameters p;
    p.procedure = FractureProcedure::tapeCut;
    p.tape.startSpeed = p.tape.endSpeed = 0;
    cutter.setFracture (p);
    double energy = 0;
    for (int i=0;i<4000;++i)
    {
        const auto output=tick(cutter,i);
        energy += output.left*output.left+output.right*output.right;
    }
    REQUIRE (energy == 0);
    REQUIRE (cutter.fractureDiagnostics().family == FractureProcedure::tapeCut);
    REQUIRE (cutter.fractureDiagnostics().cutsAdmitted == 1);
    REQUIRE (cutter.fractureDiagnostics().tapeSpeed == 0);
}

TEST_CASE ("Grain waits for its fixed guarded pool and launches immediately when complete", "[fracture][integration]")
{
    Cutter cutter;
    configure(cutter);
    OnsetDetector detector;
    Bounds bounds;
    cutter.setTransformSource(detector,bounds);
    FractureParameters p;
    p.procedure=FractureProcedure::grainCut;
    p.activity=0;
    p.grain.panSpread=0;
    cutter.setFracture(p);
    // N=320; ceil(319*2^.25)+1 + 320 scatter +31 sinc guards =732.
    constexpr int poolFrames=732;
    for(int i=0;i<poolFrames-1;++i)
    {
        const auto output=tick(cutter,i);
        REQUIRE(output.left==0);
        REQUIRE(output.right==0);
        REQUIRE(cutter.fractureDiagnostics().state==FractureState::waiting);
        REQUIRE(cutter.fractureDiagnostics().cutsAdmitted==0);
        REQUIRE(cutter.impact().diagnostics().cutsStarted==0);
    }
    tick(cutter,poolFrames-1);
    REQUIRE(cutter.fractureDiagnostics().state==FractureState::playing);
    REQUIRE(cutter.fractureDiagnostics().cutsAdmitted==1);
    REQUIRE(cutter.fractureDiagnostics().grainsLaunched==1);
    REQUIRE(cutter.fractureDiagnostics().source.firstFrame==0);
    REQUIRE(cutter.fractureDiagnostics().source.frameCount==poolFrames);
    REQUIRE(cutter.impact().diagnostics().cutsStarted==1);
    REQUIRE(tick(cutter,poolFrames).left>0);
}

TEST_CASE ("Fracture cannot invent missing transport source context", "[fracture][integration]")
{
    Cutter cutter;
    configure(cutter);
    FractureParameters p;
    p.procedure=FractureProcedure::grainCut;
    cutter.setFracture(p);
    for(int i=0;i<1000;++i)
    {
        const auto output=tick(cutter,i);
        REQUIRE(output.left==0);
        REQUIRE(output.right==0);
    }
    REQUIRE(cutter.fractureDiagnostics().state==FractureState::contextUnavailable);
    REQUIRE(cutter.fractureDiagnostics().cutsAdmitted==0);
}

TEST_CASE ("Frequency ring path changes the Classic wet chain", "[fracture][integration]")
{
    Cutter dry,wet;
    configure(dry);
    configure(wet);
    FrequencyParameters p;
    p.enabled=true;
    p.rateHz=137;
    wet.setFrequency(p);
    double difference=0;
    for(int i=0;i<4000;++i)
    {
        const auto a=tick(dry,i),b=tick(wet,i);
        difference += std::abs(a.left-b.left)+std::abs(a.right-b.right);
    }
    REQUIRE(difference>100);
}

TEST_CASE ("Classic family and disabled Frequency preserve exact old output", "[fracture][integration]")
{
    Cutter old,current;
    configure(old);
    configure(current);
    FractureParameters p;
    p.activity=1;
    p.grain.sizeMs=160;
    p.tape.startSpeed=0;
    current.setFracture(p);
    FrequencyParameters f;
    f.rateHz=-2000;
    current.setFrequency(f);
    for(int i=0;i<20000;++i)
    {
        const auto a=tick(old,i),b=tick(current,i);
        REQUIRE(a.left==b.left);
        REQUIRE(a.right==b.right);
    }
}

TEST_CASE ("Fracture retains source identity after original context metadata retires", "[fracture][integration][retirement]")
{
    struct RetiringBounds final : Cutter::SourceContext
    {
        std::int64_t retired=0;
        std::optional<Cutter::TransportBounds> sourceBoundsAt(std::int64_t frame) const override
        {
            if(frame<retired) return std::nullopt;
            return Cutter::TransportBounds{71,0,std::nullopt};
        }
    };
    for(auto family:{FractureProcedure::grainCut,FractureProcedure::tapeCut})
    {
        Cutter cutter;
        configure(cutter);
        OnsetDetector detector;
        RetiringBounds bounds;
        cutter.setTransformSource(detector,bounds);
        FractureParameters p;
        p.procedure=family;
        p.activity=0;
        p.grain.panSpread=0;
        cutter.setFracture(p);
        double energy=0;
        for(int i=0;i<3500;++i)
        {
            bounds.retired=i;
            const auto output=tick(cutter,i);
            energy+=output.left*output.left+output.right*output.right;
        }
        REQUIRE(cutter.fractureDiagnostics().source.firstFrame==0);
        REQUIRE(cutter.fractureDiagnostics().transportEpoch==71);
        REQUIRE(cutter.fractureDiagnostics().state==FractureState::playing);
        REQUIRE(cutter.fractureDiagnostics().cutsAdmitted==1);
        REQUIRE(energy>1);
    }
}

TEST_CASE ("Tape Alternate continues across one-cut phrases and resets on seek", "[fracture][integration][alternate]")
{
    Cutter cutter;
    configure(cutter);
    CutParameters common;
    common.minPhraseBars=common.maxPhraseBars=1;
    common.minPan=common.maxPan=0;
    cutter.setParameters(common);
    OnsetDetector detector;
    Bounds bounds;
    cutter.setTransformSource(detector,bounds);
    FractureParameters p;
    p.procedure=FractureProcedure::tapeCut;
    p.tape.cycle=FractureParameters::Tape::Cycle::fourBeats;
    p.tape.alternate=true;
    cutter.setFracture(p);
    for(int i=0;i<=32000;++i)
    {
        tick(cutter,i);
        if(i==0 || i==32000) REQUIRE(cutter.fractureDiagnostics().tapeSpeed==1);
        if(i==16000) REQUIRE(cutter.fractureDiagnostics().tapeSpeed==0);
    }
    cutter.resetRendering();
    auto l=.5f,r=.25f;
    cutter.process(&l,&r,1,{120,4},{0,0});
    REQUIRE(cutter.fractureDiagnostics().tapeSpeed==1);
}

namespace
{
struct Trace
{
    std::vector<float> left,right;
    std::vector<double> envelope,repeat;
    std::vector<std::uint64_t> cuts,grains;
    std::size_t allocations=0;
};

Trace renderTrace(FractureProcedure family,int chunk,bool preserveOriginalContexts)
{
    constexpr int length=30000;
    Cutter cutter;
    configure(cutter);
    cutter.setSwing(75);
    FractureParameters p;
    p.procedure=family;
    p.tape.alternate=true;
    cutter.setFracture(p);
    ImpactParameters impact;
    impact.attackMs=0;
    impact.decayMs=100;
    impact.filter.enabled=true;
    impact.filter.cutoffHz=1000;
    cutter.setImpact(impact);
    FrequencyParameters frequency;
    frequency.enabled=true;
    frequency.chance=.43f;
    frequency.envelopeHz=83;
    frequency.repeatHz=-37;
    cutter.setFrequency(frequency);
    OnsetDetector detector;
    struct EpochBounds final:Cutter::SourceContext
    {
        std::int64_t first=0;
        std::uint64_t epoch=71;
        std::optional<Cutter::TransportBounds> sourceBoundsAt(std::int64_t frame) const override
        {
            if(frame<first) return std::nullopt;
            return Cutter::TransportBounds{epoch,first,std::nullopt};
        }
    } bounds;
    cutter.setTransformSource(detector,bounds);
    Trace trace;
    trace.left.resize(length);
    trace.right.resize(length);
    trace.envelope.resize(length);
    trace.repeat.resize(length);
    trace.cuts.resize(length);
    trace.grains.resize(length);
    const auto beatAt=[](int frame)
    {
        if(frame<10000) return frame/4000.0;
        if(frame<22000) return 2.5+(frame-10000)*83.0/60/rate;
        return .75+(frame-22000)*83.0/60/rate;
    };
    testing::AllocationCounter counter;
    auto frame=0;
    while(frame<length)
    {
        if(frame==22000) { bounds.first=frame; ++bounds.epoch; }
        const auto event=frame<10000?10000:(frame<22000?22000:length);
        const auto contextEnd=std::min(event,frame+(preserveOriginalContexts?137:chunk));
        const auto beats=beatAt(frame);
        const auto bar=static_cast<std::int64_t>(std::floor(beats/4));
        cutter.beginRenderContext({frame<10000?120.0:83.0,4},{bar,beats-static_cast<double>(bar)*4},frame,0,nullptr);
        while(frame<contextEnd)
        {
            const auto end=std::min(contextEnd,frame+chunk);
            while(frame<end)
            {
                float left=static_cast<float>(.4*std::sin(frame*.17)),right=static_cast<float>(.3*std::cos(frame*.13));
                if(!cutter.ingestInputFrame(frame,{left,right})) throw std::runtime_error("trace ingestion failure");
                cutter.renderOne(frame,left,right,0,nullptr);
                const auto i=static_cast<std::size_t>(frame);
                trace.left[i]=left;
                trace.right[i]=right;
                trace.envelope[i]=cutter.impact().diagnostics().envelope;
                trace.repeat[i]=cutter.impact().diagnostics().repeatProgress;
                trace.cuts[i]=cutter.impact().diagnostics().cutsStarted;
                trace.grains[i]=cutter.fractureDiagnostics().grainsLaunched;
                ++frame;
            }
        }
        cutter.endRenderContext(0,nullptr);
    }
    trace.allocations=counter.count();
    return trace;
}
}

TEST_CASE("Fracture exact audio survives output partition with original contexts fixed","[fracture][integration][partition]")
{
    for(auto family:{FractureProcedure::grainCut,FractureProcedure::tapeCut})
    {
        const auto expected=renderTrace(family,1,true);
        REQUIRE(expected.allocations==0);
        for(int chunk:{7,137,512})
        {
            const auto actual=renderTrace(family,chunk,true);
            REQUIRE(actual.allocations==0);
            REQUIRE(actual.left==expected.left);
            REQUIRE(actual.right==expected.right);
            REQUIRE(actual.cuts==expected.cuts);
            REQUIRE(actual.grains==expected.grains);
            REQUIRE(actual.envelope==expected.envelope);
        }
    }
}

TEST_CASE("Fracture onset clock is independent of host contexts through tempo and seek","[fracture][integration][schedule]")
{
    for(auto family:{FractureProcedure::grainCut,FractureProcedure::tapeCut})
    {
        const auto expected=renderTrace(family,1,false);
        for(int chunk:{7,137,512})
        {
            const auto actual=renderTrace(family,chunk,false);
            REQUIRE(actual.cuts==expected.cuts);
            REQUIRE(actual.grains==expected.grains);
            REQUIRE(actual.envelope==expected.envelope);
            REQUIRE(actual.repeat==expected.repeat);
        }
    }
}
