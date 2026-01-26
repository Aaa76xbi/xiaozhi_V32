#pragma once

#include <cstdint>
#include <cstddef>

// 通用音频输出电平钩子：只声明，不提供默认定义
void audio_output_level_update(const int16_t* pcm, size_t samples);
