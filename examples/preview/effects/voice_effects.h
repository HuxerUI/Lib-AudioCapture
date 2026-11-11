#pragma once

#include <huxerui/audiocapture.h>

namespace voice_studio {

enum class VoicePreset {
  Original,
  Deep,
  Chipmunk,
  Robot,
  Radio,
  SlowMotion,
};

std::shared_ptr<const huxerui::audio::PcmProcessorFactory> ProcessorForPreset(VoicePreset preset);

} // namespace voice_studio
