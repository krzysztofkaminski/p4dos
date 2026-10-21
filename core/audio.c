/* Audio mixing and the hand-over between emulator and audio thread. See audio.h. */
#include "audio.h"

#define FIFO_SIZE 8192u                  /* samples, a power of two */
#define PRIME (AUDIO_RATE * 45 / 1000)   /* buffer this much before (re)starting playback */
#define SPEAKER_LEVEL 5000

static int16_t fifo[FIFO_SIZE];
static volatile uint32_t wr, rd; /* free-running; single producer, single consumer */
static volatile uint32_t speaker_hz;
static volatile int volume = AUDIO_VOLUME_DEFAULT;
static volatile int32_t gain = 32768 * AUDIO_VOLUME_DEFAULT * AUDIO_VOLUME_DEFAULT / 10000; /* Q15 */
static volatile int32_t blip; /* frames of confirmation beep still to play */

int audio_get_volume(void)
{
    return volume;
}

void audio_set_volume(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    volume = percent;
    gain = 32768 * percent * percent / 10000; /* squared: even steps sound even */
}

void audio_volume_step(int direction)
{
    audio_set_volume(volume + direction * 10);
    blip = AUDIO_RATE / 12;
}

void audio_reset(void)
{
    rd = wr;
    speaker_hz = 0;
}

void audio_push(const int16_t *samples, int n)
{
    uint32_t w = wr;
    for (int i = 0; i < n; i++) {
        if (w - rd >= FIFO_SIZE) {
            break; /* the consumer has stalled: drop the rest */
        }
        fifo[w % FIFO_SIZE] = samples[i];
        w++;
    }
    wr = w;
}

void audio_speaker(uint32_t hz)
{
    speaker_hz = hz;
}

void audio_render(int16_t *out, int frames)
{
    static int primed;
    static int32_t level;
    static uint32_t phase, blip_phase;
    const uint32_t hz = speaker_hz;
    const int32_t g = gain;
    uint32_t r = rd;

    for (int i = 0; i < frames; i++) {
        uint32_t fill = wr - r;
        if (!primed && fill >= PRIME) {
            primed = 1;
        }
        if (primed && fill) {
            level = fifo[r % FIFO_SIZE];
            r++;
        } else {
            /* Nothing to play: let the last level fade instead of stepping to zero. */
            primed = 0;
            level -= level / 64;
            if (level > -64 && level < 64) {
                level = 0;
            }
        }
        int32_t s = level;
        if (hz) {
            phase += hz * (0x100000000ull / AUDIO_RATE);
            s += (phase & 0x80000000u) ? SPEAKER_LEVEL : -SPEAKER_LEVEL;
        }
        if (blip > 0) {
            blip--;
            blip_phase += 880u * (0x100000000ull / AUDIO_RATE);
            s += (blip_phase & 0x80000000u) ? 2 * SPEAKER_LEVEL : -2 * SPEAKER_LEVEL;
        }
        s = (int32_t)(((int64_t)s * g) >> 15);
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    rd = r;
}
