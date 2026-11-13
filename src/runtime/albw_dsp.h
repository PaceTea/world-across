#ifndef ALBW_DSP_H
#define ALBW_DSP_H

#include <stdint.h>

// The DSP's audio firmware, as the game sees it.
//
// The real firmware runs on the console's Teak DSP, which this port neither has nor may ship. What a
// game needs from it is a conversation through shared memory that is well understood: per audio
// frame the game writes, into one of two regions, how each of 24 voices should play -- buffers of
// PCM or ADPCM samples, rate, gains for three mix buses, filters -- and bumps that region's frame
// counter. The DSP reads the region with the newer counter, plays one frame of 160 samples, and
// writes each voice's progress and the finished samples into the other one. This module is that
// DSP side, done on the host: decoding, resampling, filtering, mixing.
//
// One frame is 160 samples, and a frame passes every 160 * 4096 * 2 cycles of the ARM11's
// 268,111,856 Hz clock, which makes the output rate 32,728.5 Hz.

#define ALBW_DSP_SAMPLES_PER_FRAME 160u
#define ALBW_DSP_FRAME_TICKS       1310720u
#define ALBW_DSP_SAMPLE_RATE       32728

// Starts the firmware with its two shared regions at these guest addresses, from silence.
void albw_dsp_start(uint32_t region0, uint32_t region1);
void albw_dsp_stop(void);
int albw_dsp_running(void);

// One audio frame: reads the newer region, plays every voice, writes statuses and the mix into the
// other region, and hands the 160 stereo samples to the output.
void albw_dsp_tick(void);

// Where finished frames go, as interleaved stereo; NULL, the default, drops them.
void albw_dsp_set_output(void (*out)(const int16_t *stereo, unsigned frames));

// What the voices played, for the end of a run.
void albw_dsp_report(void);

#endif
