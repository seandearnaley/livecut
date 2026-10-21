// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Fracture.h"
#include "engine/LivePlayer.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

namespace livecut::engine
{
namespace
{
constexpr auto pi = std::numbers::pi;
std::uint64_t hash (std::uint64_t x)
{
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

void panMatrix (float pan, float amp, float& ll, float& lr, float& rl, float& rr)
{
    const auto angle = static_cast<double> (pan) * pi / 2;
    ll = static_cast<float> (amp * (pan < 0 ? 1.0 : std::cos (angle)));
    lr = static_cast<float> (amp * (pan < 0 ? 0.0 : std::sin (angle)));
    rl = static_cast<float> (amp * (pan > 0 ? 0.0 : -std::sin (angle)));
    rr = static_cast<float> (amp * (pan > 0 ? 1.0 : std::cos (angle)));
}
}

double fractureDraw (const CutIdentity& id, std::uint64_t tag, std::uint64_t ordinal)
{
    auto result = hash (id.seed + UINT64_C(0x9e3779b97f4a7c15));
    const auto add = [&result] (std::uint64_t value) { result = hash (result ^ hash (value)); };
    add (static_cast<std::uint64_t> (id.phraseBar));
    add (std::bit_cast<std::uint64_t> (std::fpclassify(id.blockStartUnits)==FP_ZERO ? 0.0 : id.blockStartUnits));
    add (std::bit_cast<std::uint64_t> (std::fpclassify(id.cutStartUnits)==FP_ZERO ? 0.0 : id.cutStartUnits));
    add (id.cutIndex);
    add (id.cutCount);
    return static_cast<double> (hash (result ^ tag ^ hash (ordinal)) >> 11) * 0x1p-53;
}

void TapeTrajectory::start (double a, double b, int length, FractureParameters::Tape::Curve shape)
{
    assert (std::isfinite (a) && a >= 0 && a <= 1 && std::isfinite (b) && b >= 0 && b <= 1);
    from = a;
    to = b;
    frames = std::max (length,1);
    curve = shape;
}

double TapeTrajectory::speed (int frame) const
{
    if (frames == 1)
        return frame <= 0 ? from : to;
    const auto u = std::clamp (static_cast<double> (frame) / (frames-1),0.0,1.0);
    const auto shape = curve == FractureParameters::Tape::Curve::linear ? u : u*u*(3-2*u);
    return std::clamp (from + (to-from)*shape,0.0,1.0);
}

double TapeTrajectory::position (int frame) const
{
    const auto n = static_cast<double> (std::clamp (frame,0,frames));
    if (frames == 1)
        return n*from + std::max (frame-frames,0)*to;
    const auto d = static_cast<double> (frames-1);
    const auto sum1 = n*(n-1)/2;
    const auto sum2 = n*(n-1)*(2*n-1)/6;
    const auto sum3 = sum1*sum1;
    const auto shapeSum = curve == FractureParameters::Tape::Curve::linear
                        ? sum1/d : 3*sum2/(d*d)-2*sum3/(d*d*d);
    return std::max (0.0,n*from+(to-from)*shapeSum) + std::max (frame-frames,0)*to;
}

std::int64_t TapeTrajectory::sourceFrames() const
{
    return static_cast<std::int64_t> (std::ceil (position (frames-1)))+1;
}

int GrainResampler::bucketFor (double rate)
{
    assert (std::isfinite (rate) && rate >= 0.125-1e-9 && rate <= 8.0+1e-9);
    return std::clamp (static_cast<int> (std::ceil (8*std::log2 (std::max (1.0,rate))-1e-12)),0,bucketCount-1);
}

int GrainResampler::tapsFor (double rate)
{
    const auto bucketRate = std::exp2 (bucketFor (rate)/8.0);
    return std::max (32,2*static_cast<int> (std::ceil (12*bucketRate-1e-12)));
}

void GrainResampler::prepare()
{
    coefficients.clear();
    std::size_t total = 0;
    for (int b=0;b<bucketCount;++b)
        total += static_cast<std::size_t> ((phaseCount+1)*tapsFor (std::exp2(b/8.0)));
    coefficients.reserve (total);
    for (int b=0;b<bucketCount;++b)
    {
        offsets[static_cast<std::size_t>(b)] = coefficients.size();
        const auto rate = std::exp2 (b/8.0);
        const auto taps = tapsFor(rate);
        const auto cutoff = 0.4/rate;
        for (int p=0;p<=phaseCount;++p)
        {
            const auto fraction = static_cast<double>(p)/phaseCount;
            const auto start = coefficients.size();
            auto sum = 0.0;
            for (int j=0;j<taps;++j)
            {
                const auto x = j-(taps/2-1)-fraction;
                const auto window = .42+.5*std::cos(pi*x/(taps/2))+.08*std::cos(2*pi*x/(taps/2));
                const auto sinc = std::abs(x)<1e-12 ? 2*cutoff : std::sin(2*pi*cutoff*x)/(pi*x);
                coefficients.push_back(static_cast<float>(sinc*window));
                sum += sinc*window;
            }
            for (auto i=start;i<coefficients.size();++i)
                coefficients[i]=static_cast<float>(coefficients[i]/sum);
        }
    }
}

std::span<const float> GrainResampler::coefficientsFor (double rate, int phase) const
{
    assert(phase>=0 && phase<=phaseCount && !coefficients.empty());
    const auto taps=tapsFor(rate);
    return {coefficients.data()+offsets[static_cast<std::size_t>(bucketFor(rate))]+static_cast<std::size_t>(phase*taps),static_cast<std::size_t>(taps)};
}

GrainResampler::Kernel GrainResampler::kernelFor (double rate) const
{
    assert(!coefficients.empty());
    const auto taps=tapsFor(rate);
    return {coefficients.data()+offsets[static_cast<std::size_t>(bucketFor(rate))],taps,taps/2-1};
}

std::optional<StereoFrame> GrainResampler::read (const LivePlayer& player, SourceRegion source, double position, double rate) const
{
    return read(player,source,position,kernelFor(rate));
}

std::optional<StereoFrame> GrainResampler::read (const LivePlayer& player, SourceRegion source, double position, Kernel kernel) const
{
    if (!std::isfinite(position) || position<0 || position>=static_cast<double>(source.frameCount))
        return std::nullopt;
    const auto index=static_cast<std::int64_t>(position);
    const auto phase=(position-static_cast<double>(index))*phaseCount;
    const auto row=static_cast<int>(phase);
    const auto mix=static_cast<float>(phase-row);
    const auto* a=kernel.coefficients+row*kernel.taps;
    const auto* b=a+kernel.taps;
    const auto window=player.readSourceWindow(source,index-kernel.leftGuard,static_cast<std::size_t>(kernel.taps));
    if(!window)
        return std::nullopt;
    StereoFrame out {};
    auto tap=std::size_t{0};
    const auto accumulate=[&](std::span<const StereoFrame> frames)
    {
        for(const auto& sample:frames)
        {
            const auto coefficient=a[tap]+mix*(b[tap]-a[tap]);
            out.left+=coefficient*sample.left;
            out.right+=coefficient*sample.right;
            ++tap;
        }
    };
    accumulate(window->first);
    accumulate(window->second);
    return out;
}

void FractureReader::prepare (double rate)
{
    assert(std::isfinite(rate) && rate>=1000 && rate<=384000);
    sampleRate=rate;
    resampler.prepare();
    reset();
}

void FractureReader::reset()
{
    state={};
    voices={};
    admitted=false;
    firstReady=-1;
    nextGrain=0;
}

void FractureReader::setParameters (const FractureParameters& p)
{
    parameters=p;
    if(p.procedure==FractureProcedure::classic)
    {
        state.family=p.procedure;
        state.state=FractureState::off;
        state.activeVoices=0;
        voices={};
    }
}

std::int64_t FractureReader::grainPoolFrames (const FractureParameters::Grain& p, double cents, double rate)
{
    const auto length=std::max(2L,std::lround(p.sizeMs*rate/1000));
    const auto highest=std::exp2((cents+p.pitchSpreadCents)/1200);
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(length-1)*highest))+1
         +std::lround(p.positionSpreadMs*rate/1000)+GrainResampler::tapsFor(highest)-1;
}

void FractureReader::startCut (const FractureCutContext& cut)
{
    context=cut;
    state.family=parameters.procedure;
    state.source=cut.source;
    state.transportEpoch=cut.transportEpoch;
    state.activeVoices=0;
    state.tapeSpeed=0;
    state.state=parameters.procedure==FractureProcedure::classic ? FractureState::off : FractureState::waiting;
    voices={};
    admitted=false;
    firstReady=-1;
    nextGrain=0;
    tapeSegmentAge=0;
    tapeBase=0;
    grainFrames=std::max(2,static_cast<int>(std::lround(parameters.grain.sizeMs*sampleRate/1000)));
    auto start=parameters.tape.startSpeed,end=parameters.tape.endSpeed;
    if(parameters.tape.alternate && (context.ordinal%2)!=0)
        std::swap(start,end);
    tape.start(start,end,cut.outputFrames,parameters.tape.curve);
}

void FractureReader::fail (FractureState reason)
{
    state.state=reason;
    state.activeVoices=0;
    voices={};
}

void FractureReader::retimeTape (int age, int remainingFrames)
{
    if(parameters.procedure!=FractureProcedure::tapeCut || context.completeTapeSource)
        return;
    const auto position=tapeBase+tape.position(age-tapeSegmentAge);
    const auto speed=tape.speed(age-tapeSegmentAge);
    auto end=parameters.tape.endSpeed;
    if(parameters.tape.alternate && (context.ordinal%2)!=0)
        end=parameters.tape.startSpeed;
    tapeBase=position;
    tapeSegmentAge=age;
    tape.start(speed,end,remainingFrames,parameters.tape.curve);
}

bool FractureReader::launch()
{
    auto chosen=voices.end();
    for(auto it=voices.begin();it!=voices.end();++it)
        if(!it->active) { chosen=it; break; }
    if(chosen==voices.end())
    {
        fail(FractureState::capacityError);
        return false;
    }
    auto& voice=*chosen;
    const auto cents=context.cut.cents+(2*fractureDraw(context.identity,0x475049544348ULL,nextGrain)-1)*parameters.grain.pitchSpreadCents;
    voice.rate=std::exp2(cents/1200);
    voice.kernel=resampler.kernelFor(voice.rate);
    const auto highest=std::exp2((context.cut.cents+parameters.grain.pitchSpreadCents)/1200);
    const auto scatter=std::lround(parameters.grain.positionSpreadMs*sampleRate/1000);
    const auto offset=static_cast<double>(scatter)*fractureDraw(context.identity,0x47504f534954ULL,nextGrain);
    const auto extent=std::ceil((grainFrames-1)*voice.rate);
    voice.sourceOffset=GrainResampler::leftGuard(highest)+offset+(context.reverse?extent:0);
    voice.age=0;
    voice.active=true;
    const auto pan=std::clamp(context.cut.pan+static_cast<float>((2*fractureDraw(context.identity,0x4750414eULL,nextGrain)-1)*parameters.grain.panSpread),-1.0f,1.0f);
    panMatrix(pan,context.cut.amp,voice.ll,voice.lr,voice.rl,voice.rr);
    ++nextGrain;
    ++state.grainsLaunched;
    return true;
}

bool FractureReader::process (const LivePlayer& player, int age, int soundingFrames, float& left, float& right)
{
    left=right=0;
    if(state.state==FractureState::sourceLost || state.state==FractureState::capacityError || state.state==FractureState::contextUnavailable
       || state.state==FractureState::ended || state.state==FractureState::transformBudgetExhausted)
        return false;
    if(age>=soundingFrames || soundingFrames<=0)
    {
        fail(FractureState::ended);
        return false;
    }
    if(context.completeTapeSource && age>=context.outputFrames)
    {
        fail(FractureState::transformBudgetExhausted);
        return false;
    }
    const auto grain=parameters.procedure==FractureProcedure::grainCut;
    const auto requireComplete=grain || context.completeTapeSource;
    if(requireComplete && !player.sourceAvailable(context.source))
    {
        const auto retained=player.retainedSource();
        if(admitted || retained.epoch!=context.source.epoch || context.source.firstFrame<retained.firstFrame)
            fail(FractureState::sourceLost);
        return false;
    }
    auto first=false;
    if(!admitted)
    {
        admitted=true;
        first=true;
        firstReady=age;
        ++state.cutsAdmitted;
    }
    state.state=FractureState::playing;
    if(grain)
    {
        const auto due=firstReady+static_cast<int>(std::lround(static_cast<double>(nextGrain)*grainFrames/parameters.grain.density));
        if(age>=due && !launch())
            return false;
        auto sum=0.0;
        state.activeVoices=0;
        for(auto& voice:voices)
        {
            if(!voice.active)
                continue;
            const auto position=voice.sourceOffset+(context.reverse?-1:1)*voice.age*voice.rate;
            const auto frame=resampler.read(player,context.source,position,voice.kernel);
            if(!frame)
            {
                fail(FractureState::sourceLost);
                left=right=0;
                return false;
            }
            const auto window=.5-.5*std::cos(2*pi*voice.age/(grainFrames-1));
            left+=static_cast<float>(window)*(voice.ll*frame->left+voice.rl*frame->right);
            right+=static_cast<float>(window)*(voice.lr*frame->left+voice.rr*frame->right);
            sum+=window;
            ++voice.age;
            voice.active=voice.age<grainFrames;
            state.activeVoices+=voice.active?1:0;
        }
        const auto fade=std::max(context.fadeFrames,static_cast<int>(std::ceil(.002*sampleRate)));
        const auto gain=fadeEnvelope(age,fade,soundingFrames)/std::max(1.0,sum);
        left*=static_cast<float>(gain);
        right*=static_cast<float>(gain);
    }
    else
    {
        const auto local=age-tapeSegmentAge;
        const auto position=tapeBase+tape.position(local);
        state.tapeSpeed=tape.speed(local);
        // A real stop produces silence without a stale-source/DC read.
        if(state.tapeSpeed<=0)
            return first;
        const auto coordinate=context.reverse?static_cast<double>(context.source.frameCount-1)-position:position;
        const auto frame=player.readSourceFrame(context.source,coordinate);
        if(!frame)
        {
            fail(FractureState::sourceLost);
            return false;
        }
        float ll,lr,rl,rr;
        panMatrix(context.cut.pan,context.cut.amp,ll,lr,rl,rr);
        const auto gain=static_cast<float>(std::sqrt(state.tapeSpeed))*fadeEnvelope(age,context.fadeFrames,soundingFrames);
        left=gain*(ll*frame->left+rl*frame->right);
        right=gain*(lr*frame->left+rr*frame->right);
    }
    return first;
}

} // namespace livecut::engine
