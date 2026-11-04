// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Cutter.h"
#include "engine/AllocationCounter.h"
#include "engine/PortableSignal.h"
#include "engine/PadPrograms.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
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

TEST_CASE ("Fracture phrase ranges accept independently automated reversed bounds", "[fracture][integration][phrase-bounds]")
{
    for (auto family : { FractureProcedure::grainCut, FractureProcedure::tapeCut })
    {
        Cutter ordered, reversed;
        configure (ordered);
        configure (reversed);
        FractureParameters fracture;
        fracture.procedure = family;
        ordered.setFracture (fracture);
        reversed.setFracture (fracture);
        for (int maximum : { 2, 8 })
        {
            CutParameters common;
            common.minPhraseBars = 1;
            common.maxPhraseBars = maximum;
            ordered.setParameters (common);
            std::swap (common.minPhraseBars, common.maxPhraseBars);
            reversed.setParameters (common);
            bool reachedMinimum = false, reachedMaximum = false;
            for (std::uint32_t seed = 1; seed <= 64; ++seed)
            {
                ordered.reset();
                reversed.reset();
                ordered.setSeed (seed);
                reversed.setSeed (seed);
                tick (ordered, 0);
                tick (reversed, 0);
                INFO (seed);
                REQUIRE (reversed.phraseBarCount() == ordered.phraseBarCount());
                reachedMinimum |= ordered.phraseBarCount() == 1;
                reachedMaximum |= ordered.phraseBarCount() == maximum;
            }
            REQUIRE (reachedMinimum);
            REQUIRE (reachedMaximum);
        }
    }
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

namespace
{
// FNV-1a over the raw bytes of each float.
struct Fnv1a
{
    std::uint64_t value=UINT64_C(0xcbf29ce484222325);
    void add(float sample)
    {
        const auto bits=std::bit_cast<std::uint32_t>(sample);
        for(int shift=0;shift<32;shift+=8)
        {
            value^=(bits>>shift)&0xffu;
            value*=UINT64_C(0x100000001b3);
        }
    }
};

// Six bars at 8 kHz and 120 bpm of a fixed stereo input, in 333-frame host
// buffers, through a Classic Fracture family with the page's pan spread and a
// 3 ms fade.
std::uint64_t classicFractureHash(const FractureParameters& fracture)
{
    Cutter cutter;
    cutter.prepare(rate);
    CutParameters p;
    p.minPhraseBars=1;
    p.maxPhraseBars=3;
    p.minCents=-300;
    p.maxCents=200;
    cutter.setParameters(p);
    cutter.setSeed(17);
    cutter.setFadeMilliseconds(3);
    cutter.setFracture(fracture);
    OnsetDetector detector;
    Bounds bounds;
    cutter.setTransformSource(detector,bounds);
    Fnv1a hash;
    std::array<float,333> left{},right{};
    for(int frame=0;frame<96000;frame+=333)
    {
        const auto count=std::min(333,96000-frame);
        for(int i=0;i<count;++i)
        {
            left[static_cast<std::size_t>(i)]=testing::portableSignal(frame+i,0);
            right[static_cast<std::size_t>(i)]=testing::portableSignal(frame+i,1);
        }
        const auto beat=frame/4000.0;
        const auto bar=static_cast<std::int64_t>(beat/4.0);
        cutter.process(left.data(),right.data(),count,{120,4},{bar,beat-static_cast<double>(bar)*4.0});
        for(int i=0;i<count;++i)
        {
            hash.add(left[static_cast<std::size_t>(i)]);
            hash.add(right[static_cast<std::size_t>(i)]);
        }
    }
    REQUIRE(cutter.error()==Cutter::Error::none);
    return hash.value;
}
}

TEST_CASE("Classic Fracture is bit-identical","[fracture][integration][pinned]")
{
    // Measured on the engine before the pad's Tape motions existed (d2cbbb2), on a
    // libm-free input. The MSVC values are Windows CI's, measured at b34dd71, which
    // the base-against-head fuzz shows is bit-identical to d2cbbb2 here.
    FractureParameters tape;
    tape.procedure=FractureProcedure::tapeCut;
    CHECK(classicFractureHash(tape)==testing::pinnedHash(UINT64_C(0xf1d3e685441b8157),UINT64_C(0xfe1888076797dc32)));
    tape.tape.curve=FractureParameters::Tape::Curve::linear;
    tape.tape.alternate=true;
    tape.tape.cycle=FractureParameters::Tape::Cycle::halfBeat;
    CHECK(classicFractureHash(tape)==testing::pinnedHash(UINT64_C(0x6157d6ae9f6874b2),UINT64_C(0xea63539c65af8102)));
    tape.tape.startSpeed=.8f;
    tape.tape.endSpeed=.2f;
    tape.tape.curve=FractureParameters::Tape::Curve::smooth;
    tape.tape.cycle=FractureParameters::Tape::Cycle::fourBeats;
    CHECK(classicFractureHash(tape)==testing::pinnedHash(UINT64_C(0xe03b3b0ea04e1205),UINT64_C(0x9a01b463367904f1)));
    FractureParameters grain;
    grain.procedure=FractureProcedure::grainCut;
    CHECK(classicFractureHash(grain)==testing::pinnedHash(UINT64_C(0xc62cd44318cc7a1d),UINT64_C(0xe7fb2799b21db66a)));
}

namespace
{
// Classic Tape (0, 1), Grain (2), the pad's Cut, Bounce, Glitch, Suction, Filter
// and Tape (3 to 8), Classic's CutProc11, WarpCut, SQPusher and Bounce (9 to 12),
// and the pad's Rush (13) through a restart, an idle toggle and two tempo changes,
// at a tempo whose bars aren't whole samples.
std::uint64_t pinnedEventHash (int which)
{
    constexpr double rate = 44100.0;
    Cutter cutter;
    cutter.prepare (rate);
    OnsetDetector detector;
    struct EpochBounds final : Cutter::SourceContext
    {
        std::optional<Cutter::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
        {
            if (frame < 0)
                return std::nullopt;
            return Cutter::TransportBounds { 3, 0, std::nullopt };
        }
    } bounds;
    cutter.setTransformSource (detector, bounds);
    if (which < 3)
    {
        CutParameters p;
        p.minPhraseBars = 1;
        p.maxPhraseBars = 3;
        p.minCents = -300;
        p.maxCents = 200;
        cutter.setParameters (p);
        cutter.setSeed (29);
        cutter.setFadeMilliseconds (2);
        FractureParameters f;
        f.procedure = which == 2 ? FractureProcedure::grainCut : FractureProcedure::tapeCut;
        if (which == 1)
        {
            f.tape.curve = FractureParameters::Tape::Curve::linear;
            f.tape.alternate = true;
            f.tape.cycle = FractureParameters::Tape::Cycle::halfBeat;
            f.tape.startSpeed = .9f;
            f.tape.endSpeed = .1f;
        }
        cutter.setFracture (f);
    }
    else if (which < 9 || which == 13)
    {
        const PadProgram programs[] { PadProgram::cut, PadProgram::bounce, PadProgram::glitch, PadProgram::suction, PadProgram::filter,
                                      PadProgram::tape };
        const auto m = mapPad (which == 13 ? PadProgram::rush : programs[which - 3], .3f, .75f, {});
        cutter.setProcedure (m.procedure);
        cutter.setUnitsPerBar (m.unitsPerBar);
        cutter.setSeed (m.seed);
        cutter.setParameters (m.cut);
        cutter.setCrusher (m.crusher);
        cutter.setImpact (m.impact);
        cutter.setFracture (m.fracture);
    }
    else
    {
        // Classic's four procedures, pitched, panned and scaled, at fades of 0 and 2 ms.
        CutParameters p;
        p.minPhraseBars = 1;
        p.maxPhraseBars = 2;
        p.minCents = -400;
        p.maxCents = 300;
        p.minPan = -.6f;
        p.maxPan = .5f;
        p.minAmp = .4f;
        p.maxRepeats = 3;
        p.straight = which == 11;
        cutter.setProcedure (static_cast<Procedure> (which - 9));
        cutter.setUnitsPerBar (which == 10 ? 8 : 16);
        cutter.setSeed (41);
        cutter.setParameters (p);
        cutter.setFadeMilliseconds (which % 2 == 0 ? 2.0 : 0.0);
    }
    std::uint64_t hash = UINT64_C (0xcbf29ce484222325);
    const auto add = [&hash] (float sample)
    {
        const auto bits = std::bit_cast<std::uint32_t> (sample);
        for (int shift = 0; shift < 32; shift += 8)
        {
            hash ^= (bits >> shift) & 0xffu;
            hash *= UINT64_C (0x100000001b3);
        }
    };
    // Ten bars of 4/4 from 97 bpm, a bar of fractional samples, with a restart, an
    // idle toggle and two tempo changes, in 512-frame host buffers.
    double beats = 0.0, bpm = 97.0;
    std::int64_t frame = 0;
    std::array<float, 512> left {}, right {};
    bool restarted = false, idled = false, resumed = false, faster = false, slower = false;
    while (beats < 40.0)
    {
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            left[i] = testing::portableSignal (frame + static_cast<std::int64_t> (i), 0);
            right[i] = testing::portableSignal (frame + static_cast<std::int64_t> (i), 1);
        }
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        cutter.process (left.data(), right.data(), 512, { bpm, 4.0 }, { bar, beats - 4.0 * static_cast<double> (bar) });
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            add (left[i]);
            add (right[i]);
        }
        frame += 512;
        beats += 512.0 * bpm / 60.0 / rate;
        if (! restarted && beats > 9.3) { cutter.restartAtNextUnit(); restarted = true; }
        if (! idled && beats > 16.4) { cutter.setIdle (true); idled = true; }
        if (! resumed && beats > 18.5) { cutter.setIdle (false); resumed = true; }
        if (! faster && beats > 24.7) { bpm = 103.0; faster = true; }
        if (! slower && beats > 33.2) { bpm = 91.0; slower = true; }
    }
    return hash;
}
} // namespace

TEST_CASE("Classic Fracture and the built programs are bit-identical through restarts, idle and tempo changes","[fracture][integration][pinned]")
{
    // Measured on the engine before Tape (d2cbbb2), and equal on this branch, on a
    // libm-free input. The MSVC values are Windows CI's, measured at b34dd71. Tape (8)
    // and Classic's procedures (9 to 12) were measured on pad/tape before Rush
    // (b34dd71), and Rush (13) as it was built; their MSVC values are Windows CI's at
    // 8c10c43. Rush (13) was re-measured when its joins began to crossfade (MSVC at c0c59e1).
    constexpr std::array<std::uint64_t,14> expected {
        testing::pinnedHash(UINT64_C(0x2f582588aa69dda7),UINT64_C(0x1f3d82b817cc3135)), testing::pinnedHash(UINT64_C(0xa09e8c6a97ac05eb),UINT64_C(0x8d0225cdfc3b26f0)), testing::pinnedHash(UINT64_C(0x8dfd275471cb838b),UINT64_C(0xa315ceaf0d35f004)), testing::pinnedHash(UINT64_C(0xbd765792b25f5b6a),UINT64_C(0x3d812833c479f0d2)),
        testing::pinnedHash(UINT64_C(0x102182ecbb1e9824),UINT64_C(0x3227a7eafd96dc39)), testing::pinnedHash(UINT64_C(0xc802fe7c3ef2261e),UINT64_C(0x71987b5803b1b4e5)), testing::pinnedHash(UINT64_C(0xf16d4f18e2706587),UINT64_C(0x1af7d9c363b0f056)), testing::pinnedHash(UINT64_C(0x53c8983cba5b2842),UINT64_C(0x4b629cf30fab193d)),
        testing::pinnedHash(UINT64_C(0x582da3bde16cd917),UINT64_C(0xf1220989e57a4a9f)), testing::pinnedHash(UINT64_C(0xf471244cf27e91e7),UINT64_C(0x2628a4760968298d)), testing::pinnedHash(UINT64_C(0x08bbb0f7a7d88d38),UINT64_C(0x6a0984dc22ec138f)), testing::pinnedHash(UINT64_C(0x48b6b22fc72be4e4),UINT64_C(0x8d01597ed4912dbe)),
        testing::pinnedHash(UINT64_C(0xf2958df8ef88e9b5),UINT64_C(0x22effe140e38afa6)), 
        testing::pinnedHash(UINT64_C(0x8bd9eaa04dee1db7),UINT64_C(0xc20c02f50e06a2fc)) };
    for(int which=0;which<14;++which)
    {
        CAPTURE(which);
        CHECK(pinnedEventHash(which)==expected[static_cast<std::size_t>(which)]);
    }
}
