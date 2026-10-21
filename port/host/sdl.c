/*
 * SDL2 front end of the host port: a 1280x800 window showing exactly what the panel shows,
 * the PC keyboard fed through the same USB-HID path as on the board, the mouse as the
 * serial mouse on COM1, and Sound Blaster and PC speaker sound.
 *
 * Click the window to capture the mouse; Ctrl+F10 releases it; close the window to quit.
 */
#include <SDL.h>
#include <string.h>

#include "audio.h"
#include "host.h"
#include "pc.h"
#include "video.h"

static int active;

int host_sdl_active(void) { return active; }

static void audio_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    audio_render((int16_t *)stream, len / 2);
}

int host_sdl_run(void)
{
    static uint16_t fb[SCREEN_W * SCREEN_H];
    const render_target_t target = {.fb = fb, .stride = SCREEN_W, .rotate = ROT_NONE};
    uint8_t keys[6] = {0}, mods = 0;
    int buttons = 0, grabbed = 0, quit = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window *win = SDL_CreateWindow("P4DOS", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : 0;
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                               SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H)
                           : 0;
    if (!tex) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 2;
    }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);

    SDL_AudioSpec want = {.freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 1,
                          .samples = 512, .callback = audio_cb};
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(0, 0, &want, 0, 0);
    if (audio) {
        SDL_PauseAudioDevice(audio, 0);
    }

    active = 1;
    mouse_present = 1;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                int sc = e.key.keysym.scancode; /* SDL scancodes are USB HID usages */
                int down = e.type == SDL_KEYDOWN;
                if (e.key.repeat) {
                    break;
                }
                if (down && sc == SDL_SCANCODE_F10 && (mods & 0x11)) {
                    grabbed = 0;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    break;
                }
                if (sc >= 0xE0 && sc <= 0xE7) {
                    mods = down ? (uint8_t)(mods | (1 << (sc - 0xE0)))
                                : (uint8_t)(mods & ~(1 << (sc - 0xE0)));
                } else if (sc >= 4 && sc < 0x66) {
                    uint8_t *slot = memchr(keys, down ? 0 : sc, 6);
                    if (slot && !(down && memchr(keys, sc, 6))) {
                        *slot = down ? (uint8_t)sc : 0;
                    }
                }
                kbd_hid_report(mods, keys, 6);
                break;
            }
            case SDL_MOUSEMOTION:
                if (grabbed) {
                    mouse_event(e.motion.xrel, e.motion.yrel, buttons);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int bit = e.button.button == SDL_BUTTON_LEFT ? 1
                          : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
                if (!grabbed) {
                    if (e.type == SDL_MOUSEBUTTONDOWN) {
                        grabbed = 1;
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                    }
                    break;
                }
                buttons = e.type == SDL_MOUSEBUTTONDOWN ? buttons | bit : buttons & ~bit;
                mouse_event(0, 0, buttons);
                break;
            }
            default: break;
            }
        }

        uint64_t t0 = hal_time_ns();
        while (hal_time_ns() - t0 < 8000000ull) {
            pc_step();
        }

        int r0, r1;
        if (render_update(&target, hal_time_ns(), 0, &r0, &r1)) {
            SDL_UpdateTexture(tex, 0, fb, SCREEN_W * 2);
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, 0, 0);
        SDL_RenderPresent(ren);
    }
    disk_flush();
    SDL_Quit();
    return 0;
}
