#include "voice_effects.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <numbers>
#include <utility>

namespace voice_studio {
namespace {

using namespace huxerui::audio;
using huxerui::media::AudioFormat;
using Output = CaptureResult<std::optional<PcmBlock>>;
using Factory = std::shared_ptr<const PcmProcessorFactory>;

constexpr float pcm_scale = 32768.0F;
constexpr float tau = 2.0F * std::numbers::pi_v<float>;

std::int16_t ToPcm(float sample) {
  if (!std::isfinite(sample)) return 0;
  return static_cast<std::int16_t>(std::lround(std::clamp(sample * pcm_scale, -pcm_scale, pcm_scale - 1.0F)));
}

struct InterleavedSamples {
  struct Channel {
    float* first;
    std::size_t stride;

    float& operator[](std::size_t frame) const { return first[frame * stride]; }
  };

  float* samples;
  std::size_t channels;

  Channel operator[](std::size_t channel) const { return {samples + channel, channels}; }
};

class StretchProcessor final : public PcmProcessor {
public:
  StretchProcessor(VoicePreset preset, const AudioFormat& format)
      : channels_(format.ChannelCount()), slows_down_(preset == VoicePreset::SlowMotion) {
    const auto rate = static_cast<float>(format.sample_rate);
    stretch_.presetDefault(static_cast<int>(channels_), rate, true);
    if (!slows_down_) {
      const bool deep = preset == VoicePreset::Deep;
      stretch_.setTransposeSemitones(deep ? -5.0F : 7.0F);
      stretch_.setFormantSemitones(deep ? -3.0F : 4.0F, true);
      stretch_.setFormantBase(170.0F / rate);
    }
    prime_frames_ = stretch_.outputSeekLength(PlaybackRate());
    input_.reserve(std::max(block_frames, prime_frames_) * channels_);
  }

  PcmTiming Timing() const noexcept override {
    return slows_down_ ? PcmTiming::StreamTime : PcmTiming::CaptureTime;
  }

  CaptureResult<void> Process(std::span<const std::int16_t> pcm) override {
    while (!pcm.empty()) {
      const auto target = (primed_ ? block_frames : prime_frames_) * channels_;
      const auto count = std::min(pcm.size(), target - input_.size());
      std::ranges::transform(pcm.first(count), std::back_inserter(input_),
                             [](std::int16_t sample) { return sample / pcm_scale; });
      input_frames_ += count / channels_;
      pcm = pcm.subspan(count);
      if (input_.size() == target) {
        if (primed_) Render();
        else Prime();
      }
    }
    return CaptureResult<void>::Success();
  }

  Output TryRead() override {
    if (ready_.empty()) return Output::Success(std::nullopt);
    auto block = std::move(ready_.front());
    ready_.pop_front();
    return Output::Success(std::move(block));
  }

  CaptureResult<void> Finish() override {
    if (!input_frames_) return CaptureResult<void>::Success();
    if (!primed_) {
      input_.resize(prime_frames_ * channels_);
      Prime();
    } else if (!input_.empty()) {
      Render();
    }

    // Priming holds one window of audio inside Stretch. A short take uses the same
    // priming window padded with silence, then keeps only its recorded duration.
    const auto tail_frames = static_cast<int>(OutputFrames(prime_frames_));
    output_.resize(tail_frames * channels_);
    stretch_.flush(InterleavedSamples{output_.data(), channels_}, tail_frames, PlaybackRate());
    Enqueue(static_cast<std::size_t>(OutputFrames(std::min<std::uint64_t>(input_frames_, prime_frames_))));
    return CaptureResult<void>::Success();
  }

private:
  float PlaybackRate() const noexcept { return slows_down_ ? 0.8F : 1.0F; }

  std::uint64_t OutputFrames(std::uint64_t input_frames) const noexcept {
    return slows_down_ ? input_frames + input_frames / 4 : input_frames;
  }

  void Prime() {
    stretch_.outputSeek(InterleavedSamples{input_.data(), channels_}, prime_frames_);
    primed_ = true;
    input_.clear();
  }

  void Render() {
    const auto frames = static_cast<int>(input_.size() / channels_);
    const auto output_frames = static_cast<int>(OutputFrames(input_frames_) - OutputFrames(input_frames_ - frames));
    output_.resize(output_frames * channels_);
    stretch_.process(InterleavedSamples{input_.data(), channels_}, frames,
                     InterleavedSamples{output_.data(), channels_}, output_frames);
    input_.clear();
    Enqueue(output_frames);
  }

  void Enqueue(std::size_t frames) {
    auto& block = ready_.emplace_back(frames * channels_);
    std::ranges::transform(std::span(output_).first(block.size()), block.begin(), ToPcm);
  }

  static constexpr int block_frames = 1024;
  const std::size_t channels_;
  const bool slows_down_;
  signalsmith::stretch::SignalsmithStretch<float> stretch_;
  int prime_frames_ = 0;
  bool primed_ = false;
  std::uint64_t input_frames_ = 0;
  std::vector<float> input_;
  std::vector<float> output_;
  std::deque<PcmBlock> ready_;
};

class VoiceFilter final : public PcmProcessor {
public:
  VoiceFilter(VoicePreset preset, const AudioFormat& format)
      : preset_(preset), channels_(format.ChannelCount()),
        phase_step_(tau * 68.0F / format.sample_rate),
        high_pass_(1.0F - std::exp(-tau * 420.0F / format.sample_rate)),
        low_pass_(1.0F - std::exp(-tau * 2800.0F / format.sample_rate)) {}

  PcmTiming Timing() const noexcept override { return PcmTiming::CaptureTime; }

  CaptureResult<void> Process(std::span<const std::int16_t> pcm) override {
    auto& block = output_.emplace(pcm.size());
    if (preset_ == VoicePreset::Robot) {
      for (std::size_t frame = 0; frame < pcm.size(); frame += channels_) {
        const auto modulation = 0.22F + 0.78F * std::sin(phase_);
        for (std::size_t channel = 0; channel < channels_; ++channel) {
          block[frame + channel] = ToPcm(pcm[frame + channel] / pcm_scale * modulation);
        }
        phase_ += phase_step_;
        if (phase_ >= tau) phase_ -= tau;
      }
    } else {
      for (std::size_t sample = 0; sample < pcm.size(); ++sample) {
        auto& filter = filters_[sample % channels_];
        const auto input = pcm[sample] / pcm_scale;
        filter.bass += high_pass_ * (input - filter.bass);
        filter.band += low_pass_ * (input - filter.bass - filter.band);
        block[sample] = ToPcm(0.82F * std::tanh(2.5F * filter.band));
      }
    }
    return CaptureResult<void>::Success();
  }

  Output TryRead() override { return Output::Success(std::exchange(output_, std::nullopt)); }

  CaptureResult<void> Finish() override { return CaptureResult<void>::Success(); }

private:
  struct RadioFilter {
    float bass = 0.0F;
    float band = 0.0F;
  };

  const VoicePreset preset_;
  const std::size_t channels_;
  const float phase_step_;
  const float high_pass_;
  const float low_pass_;
  float phase_ = 0.0F;
  std::array<RadioFilter, 2> filters_{};
  std::optional<PcmBlock> output_;
};

Factory MakeFactory(VoicePreset preset) {
  return std::make_shared<const PcmProcessorFactory>([preset](const AudioFormat& format) -> std::unique_ptr<PcmProcessor> {
    if (preset == VoicePreset::Robot || preset == VoicePreset::Radio)
      return std::make_unique<VoiceFilter>(preset, format);
    return std::make_unique<StretchProcessor>(preset, format);
  });
}

} // namespace

std::shared_ptr<const PcmProcessorFactory> ProcessorForPreset(VoicePreset preset) {
  static const std::array<Factory, 6> factories{
      nullptr, MakeFactory(VoicePreset::Deep), MakeFactory(VoicePreset::Chipmunk),
      MakeFactory(VoicePreset::Robot), MakeFactory(VoicePreset::Radio), MakeFactory(VoicePreset::SlowMotion),
  };
  return factories.at(static_cast<std::size_t>(preset));
}

} // namespace voice_studio
