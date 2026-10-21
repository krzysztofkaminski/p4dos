/*
 * Sound output: one mono 16-bit stream at AUDIO_RATE that mixes the Sound Blaster's
 * digital output with the PC speaker tone. The emulator thread produces, the platform's
 * audio thread (or callback) consumes.
 */
#ifndef P4DOS_AUDIO_H
#define P4DOS_AUDIO_H

#include <stdint.h>

#define AUDIO_RATE 22050

void audio_reset(void);
void audio_push(const int16_t *samples, int n); /* emulator thread; drops when full */
void audio_speaker(uint32_t hz);                /* PC speaker tone, 0 = off */
void audio_render(int16_t *out, int frames);    /* audio thread */

/* Master volume, 0-100 in steps of 10. A step is confirmed by a short beep. */
#define AUDIO_VOLUME_DEFAULT 60
int audio_get_volume(void);
void audio_set_volume(int percent);
void audio_volume_step(int direction);          /* +1 louder, -1 quieter */

#endif
