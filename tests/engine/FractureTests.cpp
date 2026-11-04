// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Fracture.h"
#include "engine/LivePlayer.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("Readonly history windows validate before exposing wrapped spans","[fracture][source]")
{
    AudioHistory history;
    REQUIRE(history.prepare(8));
    const auto epoch=history.retainedRegion().epoch;
    for(int i=0;i<12;++i)
        REQUIRE(history.append({static_cast<float>(i),-static_cast<float>(i)}));
    const SourceRegion region{epoch,4,8};
    const auto window=history.readWindow(region,2,4);
    REQUIRE(window);
    REQUIRE(window->first.size()==2);
    REQUIRE(window->second.size()==2);
    REQUIRE(window->first[0].left==6);
    REQUIRE(window->second[1].left==9);
    REQUIRE_FALSE(history.readWindow(region,-1,2));
    REQUIRE_FALSE(history.readWindow(region,0,0));
    REQUIRE_FALSE(history.readWindow(region,7,2));
    REQUIRE_FALSE(history.readWindow({epoch,0,12},0,8));
    REQUIRE_FALSE(history.readWindow({epoch,4,16},0,9));
    REQUIRE(history.reset());
    REQUIRE_FALSE(history.readWindow(region,0,1));
}

TEST_CASE("Tape discrete trajectory matches direct sums through true zero","[fracture][tape]")
{
    for(auto curve:{FractureParameters::Tape::Curve::linear,FractureParameters::Tape::Curve::smooth})
        for(auto from:{0.0,.3,1.0})
            for(auto to:{0.0,.7,1.0})
                for(int length:{1,2,3,137,8192})
                {
                    TapeTrajectory trajectory;
                    trajectory.start(from,to,length,curve);
                    double sum=0;
                    for(int frame=0;frame<length;++frame)
                    {
                        REQUIRE_THAT(trajectory.position(frame),WithinAbs(sum,1e-8));
                        REQUIRE(trajectory.speed(frame)>=0);
                        REQUIRE(trajectory.speed(frame)<=1);
                        sum+=trajectory.speed(frame);
                    }
                    const auto last=sum-trajectory.speed(length-1);
                    REQUIRE(trajectory.sourceFrames()==static_cast<std::int64_t>(std::ceil(last-1e-9))+1);
                }
    TapeTrajectory stopped;
    stopped.start(0,0,1000,FractureParameters::Tape::Curve::smooth);
    REQUIRE(stopped.sourceFrames()==1);
    REQUIRE(stopped.position(100000)==0);
}

TEST_CASE("Grain sinc tables bound fold-band response for all rate buckets","[fracture][sinc]")
{
    GrainResampler resampler;
    resampler.prepare();
    constexpr auto pi=std::numbers::pi;
    double maximum=0;
    for(int bucket=0;bucket<GrainResampler::bucketCount;++bucket)
    {
        const auto rate=std::exp2(bucket/8.0);
        REQUIRE(GrainResampler::tapsFor(rate)<=192);
        for(int phase:{0,32,128,224,256})
        {
            const auto coefficients=resampler.coefficientsFor(rate,phase);
            double dc=0;
            for(auto value:coefficients) dc+=value;
            REQUIRE_THAT(dc,WithinAbs(1,1e-6));
            for(int bin=0;bin<=128;++bin)
            {
                const auto frequency=.5/rate+(.5-.5/rate)*bin/128;
                std::complex<double> response{};
                for(std::size_t tap=0;tap<coefficients.size();++tap)
                    response+=static_cast<double>(coefficients[tap])*std::polar(1.0,-2*pi*frequency*static_cast<double>(tap));
                maximum=std::max(maximum,std::abs(response));
            }
        }
    }
    // 50dB attenuation at and beyond output Nyquist, not an alias-free claim.
    REQUIRE(maximum<std::pow(10.0,-50.0/20));
}

TEST_CASE("Grain sinc reads use complete guards and preserve stereo DC","[fracture][sinc]")
{
    LivePlayer player;
    player.prepare(2048);
    const auto source=player.sourceAtFrame(0);
    for(int i=0;i<2048;++i) REQUIRE(player.ingestFrame(.25f,-.5f));
    GrainResampler resampler;
    resampler.prepare();
    for(double rate:{.125,1.0,1.19,2.0,4.0,8.0})
        for(double phase:{0.0,.125,.5,.875})
        {
            const auto frame=resampler.read(player,source,128+phase,rate);
            REQUIRE(frame);
            REQUIRE_THAT(frame->left,WithinAbs(.25,1e-6));
            REQUIRE_THAT(frame->right,WithinAbs(-.5,1e-6));
        }
    REQUIRE_FALSE(resampler.read(player,source,0,8));
    REQUIRE_FALSE(resampler.read(player,source,2047,8));
}

TEST_CASE("Grain pool includes sinc guards at default and maximum pitch","[fracture][grain]")
{
    FractureParameters::Grain p;
    REQUIRE(FractureReader::grainPoolFrames(p,0,8000)==732);
    REQUIRE(FractureReader::grainPoolFrames(p,0,48000)==4235);
    p.sizeMs=160;
    p.positionSpreadMs=250;
    p.pitchSpreadCents=1200;
    REQUIRE(FractureReader::grainPoolFrames(p,2400,48000)==73624);
    REQUIRE(GrainResampler::leftGuard(8)==95);
    REQUIRE(GrainResampler::rightGuard(8)==96);
}

TEST_CASE("Grain bounded overlap does not allocate or steal voices","[fracture][grain][allocation]")
{
    LivePlayer player;
    player.prepare(16000);
    const auto source=player.sourceAtFrame(0);
    for(int i=0;i<16000;++i) REQUIRE(player.ingestFrame(.5f,.5f));
    FractureReader reader;
    reader.prepare(8000);
    FractureParameters p;
    p.procedure=FractureProcedure::grainCut;
    p.grain.density=4;
    p.grain.pitchSpreadCents=0;
    p.grain.positionSpreadMs=0;
    p.grain.panSpread=0;
    reader.setParameters(p);
    FractureCutContext c;
    c.source={source.epoch,0,FractureReader::grainPoolFrames(p.grain,0,8000)};
    c.outputFrames=12000;
    c.fadeFrames=16;
    reader.startCut(c);
    std::size_t allocations=0,voices=0;
    double maximum=0;
    {
        testing::AllocationCounter counter;
        for(int i=0;i<12000;++i)
        {
            float l=0,r=0;
            reader.process(player,i,12000,l,r);
            maximum=std::max(maximum,std::max(std::abs(static_cast<double>(l)),std::abs(static_cast<double>(r))));
            voices=std::max(voices,reader.diagnostics().activeVoices);
        }
        allocations=counter.count();
    }
    REQUIRE(allocations==0);
    REQUIRE(voices<=5);
    REQUIRE(maximum<=.50001);
    REQUIRE(maximum>.49);
    REQUIRE(reader.diagnostics().state==FractureState::playing);
    REQUIRE(reader.diagnostics().cutsAdmitted==1);
    REQUIRE(reader.diagnostics().grainsLaunched>100);
}

TEST_CASE("A Grain gate ending during Waiting cancels admission and pending launch","[fracture][grain]")
{
    LivePlayer player;
    player.prepare(16000);
    FractureReader reader;
    reader.prepare(8000);
    FractureParameters p;
    p.procedure=FractureProcedure::grainCut;
    reader.setParameters(p);
    FractureCutContext c;
    c.source={player.sourceAtFrame(0).epoch,0,732};
    c.outputFrames=100;
    reader.startCut(c);
    for(int i=0;i<800;++i)
    {
        REQUIRE(player.ingestFrame(.5f,.25f));
        float l=0,r=0;
        REQUIRE_FALSE(reader.process(player,i,100,l,r));
        REQUIRE(l==0);
        REQUIRE(r==0);
    }
    REQUIRE(reader.diagnostics().cutsAdmitted==0);
    REQUIRE(reader.diagnostics().grainsLaunched==0);
    REQUIRE(reader.diagnostics().state==FractureState::ended);
}

TEST_CASE("Tape tempo extension remains causal while transformed output budget stays frozen","[fracture][tape]")
{
    for(bool complete:{false,true})
    {
        LivePlayer player;
        player.prepare(20000);
        const auto epoch=player.sourceAtFrame(0).epoch;
        if(complete)
            for(int i=0;i<4000;++i) REQUIRE(player.ingestFrame(.5f,.25f));
        FractureReader reader;
        reader.prepare(8000);
        FractureParameters p;
        p.procedure=FractureProcedure::tapeCut;
        reader.setParameters(p);
        FractureCutContext context;
        context.outputFrames=4000;
        context.fadeFrames=16;
        context.completeTapeSource=complete;
        context.reverse=complete;
        TapeTrajectory frozen;
        frozen.start(1,0,4000,p.tape.curve);
        context.source={epoch,0,complete?frozen.sourceFrames():20000};
        reader.startCut(context);
        double lateEnergy=0;
        for(int age=0;age<7000;++age)
        {
            if(!complete) REQUIRE(player.ingestFrame(.5f,.25f));
            if(age==2000) reader.retimeTape(age,5000);
            float l=0,r=0;
            reader.process(player,age,7000,l,r);
            if(age>=4500 && age<6000) lateEnergy+=l*l+r*r;
            REQUIRE(reader.diagnostics().state!=FractureState::sourceLost);
        }
        if(complete)
        {
            REQUIRE(lateEnergy==0);
            REQUIRE(reader.diagnostics().state==FractureState::transformBudgetExhausted);
        }
        else
        {
            REQUIRE(lateEnergy>1);
            REQUIRE(reader.diagnostics().state==FractureState::playing);
        }
    }
}

TEST_CASE("Unity Tape reproduces the existing reader envelope and stereo matrix exactly","[fracture][tape]")
{
    LivePlayer classic,source;
    classic.prepare(4000);
    source.prepare(4000);
    classic.setFadeSamples(16);
    const std::array<Cut,1> cuts{{{1,1,.2f,.8f,0}}};
    classic.startBlock(cuts,4000);
    FractureReader reader;
    reader.prepare(8000);
    FractureParameters p;
    p.procedure=FractureProcedure::tapeCut;
    p.tape.startSpeed=p.tape.endSpeed=1;
    reader.setParameters(p);
    FractureCutContext c;
    c.cut=cuts[0];
    c.source=source.sourceAtFrame(0);
    c.outputFrames=4000;
    c.fadeFrames=16;
    reader.startCut(c);
    for(int age=0;age<4000;++age)
    {
        auto left=static_cast<float>(std::sin(age*.13)),right=static_cast<float>(std::cos(age*.07));
        REQUIRE(source.ingestFrame(left,right));
        classic.process(left,right);
        float actualLeft=0,actualRight=0;
        reader.process(source,age,4000,actualLeft,actualRight);
        REQUIRE(actualLeft==left);
        REQUIRE(actualRight==right);
    }
}

TEST_CASE("Actual sinc interpolation reduces an 8x folded tone relative to linear","[fracture][sinc]")
{
    constexpr int length=4096,sourceBin=293;
    constexpr auto pi=std::numbers::pi;
    LivePlayer player;
    player.prepare(40000);
    const auto source=player.sourceAtFrame(0);
    for(int i=0;i<40000;++i)
    {
        const auto value=static_cast<float>(.1*std::sin(2*pi*sourceBin*i/length));
        REQUIRE(player.ingestFrame(value,-value));
    }
    GrainResampler resampler;
    resampler.prepare();
    const auto kernel=resampler.kernelFor(8);
    double filtered=0,linear=0;
    for(int i=0;i<length;++i)
    {
        const auto position=128.0+8*i;
        const auto a=resampler.read(player,source,position,kernel);
        const auto b=player.readSourceFrame(source,position);
        REQUIRE(a);
        REQUIRE(b);
        filtered+=a->left*a->left;
        linear+=b->left*b->left;
    }
    REQUIRE(linear>10);
    REQUIRE(std::sqrt(filtered/linear)<std::pow(10.0,-50.0/20));
}

TEST_CASE("A quadratic brake slows as 1 - u squared and stops at exactly 0","[fracture][tape]")
{
    for(int length:{2,3,137,12000})
    {
        CAPTURE(length);
        TapeTrajectory brake;
        brake.start(1,0,length,FractureParameters::Tape::Curve::quadratic);
        double sum=0;
        for(int frame=0;frame<length;++frame)
        {
            const auto u=static_cast<double>(frame)/(length-1);
            REQUIRE_THAT(brake.speed(frame),WithinAbs(1-u*u,1e-12));
            REQUIRE_THAT(brake.position(frame),WithinAbs(sum,1e-9*std::max(1.0,sum)));
            sum+=brake.speed(frame);
        }
        REQUIRE(brake.speed(0)==1);
        REQUIRE(brake.speed(length-1)==0);
        REQUIRE(brake.position(length+50)==brake.position(length)); // stopped stays put
    }
    // A brake re-timed from part speed keeps the same shape from where it is.
    TapeTrajectory retimed;
    retimed.start(.6,0,101,FractureParameters::Tape::Curve::quadratic);
    REQUIRE_THAT(retimed.speed(50),WithinAbs(.6*(1-.25),1e-12));
    REQUIRE(retimed.speed(100)==0);
}

TEST_CASE("A catch-up winds up from still and lands exactly on the track","[fracture][tape]")
{
    for(int length:{1,2,3,3000,12000,96000})
    {
        CAPTURE(length);
        TapeTrajectory spin;
        spin.start(0,1,length,FractureParameters::Tape::Curve::catchUp);
        REQUIRE(spin.speed(0)==0);
        REQUIRE(spin.position(0)==0);
        double peak=0;
        int peakFrame=0;
        for(int frame=0;frame<=length;++frame)
        {
            CAPTURE(frame);
            const auto u=static_cast<double>(frame)/length;
            const auto position=spin.position(frame);
            // Never ahead of the newest recorded frame: frame n reads at most frame n.
            REQUIRE(position<=frame);
            REQUIRE_THAT(spin.speed(frame),WithinAbs(6*u-9*u*u+4*u*u*u,1e-12));
            REQUIRE_THAT(position,WithinAbs(length*(3*u*u-3*u*u*u+u*u*u*u),1e-9*length));
            if(frame<length)
                REQUIRE(std::abs(spin.position(frame+1)-position-spin.speed(frame))<=3.0/length+1e-9);
            if(spin.speed(frame)>peak)
            {
                peak=spin.speed(frame);
                peakFrame=frame;
            }
        }
        // It lands on the track exactly, and plays on it at speed 1.
        REQUIRE(spin.position(length)==length);
        REQUIRE(spin.speed(length)==1);
        REQUIRE(spin.position(length+777)==length+777);
        REQUIRE(spin.speed(length+777)==1);
        if(length>=3000)
        {
            REQUIRE_THAT(peak,WithinAbs(1.25,1e-6));
            REQUIRE(std::abs(peakFrame-length/2)<=1);
            REQUIRE(std::abs(spin.speed(length-1)-1)<=1e-6);
            REQUIRE(spin.position(length-1)>length-1-1e-6);
            REQUIRE(spin.sourceFrames()==length);
        }
    }
}
