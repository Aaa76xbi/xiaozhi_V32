#include "audio/codecs/audio_led_hook.h"
#include "boards/yunxi-spacemans/audio_led_meter.h"

// 板级实现：将通用钩子转发到具体的音量律动实现
void audio_output_level_update(const int16_t* pcm, size_t samples) {
    audio_led_meter_update(pcm, samples);
}
