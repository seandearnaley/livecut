// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpectralKernel.h"
#include <juce_dsp/juce_dsp.h>
namespace livecut::spectral
{
namespace
{
class JuceFft final : public FftPlan
{
  public:
    explicit JuceFft (int order) : fft (order) {}
    void forward (float *data) noexcept override
    {
        fft.performRealOnlyForwardTransform (data, true);
    }
    void inverse (float *data) noexcept override
    {
        fft.performRealOnlyInverseTransform (data);
    }

  private:
    juce::dsp::FFT fft;
};
} // namespace
std::unique_ptr<FftPlan> makeJuceFft (int order)
{
    return std::make_unique<JuceFft> (order);
}
} // namespace livecut::spectral
