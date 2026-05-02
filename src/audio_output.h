#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <cstddef>
#include <cstdint>

struct AudioOutput;

AudioOutput* audio_output_open(int sample_rate, int channels);
void audio_output_write(AudioOutput* ctx, const uint8_t* data, size_t bytes);
void audio_output_close(AudioOutput* ctx);

#endif // AUDIO_OUTPUT_H
