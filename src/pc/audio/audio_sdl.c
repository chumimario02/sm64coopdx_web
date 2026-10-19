#include <stdio.h>

#include <SDL2/SDL.h>

#include "audio_api.h"

static SDL_AudioDeviceID dev;

static bool audio_sdl_init(void) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL init error: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = NULL;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudio error: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

static int audio_sdl_buffered(void) {
    return SDL_GetQueuedAudioSize(dev) / 4;
}

static int audio_sdl_get_desired_buffered(void) {
    return 1100;
}

static void audio_sdl_play(const uint8_t *buf, size_t len) {
    if (audio_sdl_buffered() < 6000) {
#ifdef TARGET_WEB
        // The mods' audio (songs, sound effects) is mixed into the game's own
        // output here: the browser build has a single audio queue / device, so
        // miniaudio is pulled from this same callback (smlua_audio_utils.c).
        extern void smlua_audio_web_mix(int16_t* samples, unsigned int frames);
        smlua_audio_web_mix((int16_t*) buf, (unsigned int) (len / 4));
#endif
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(dev, buf, len);
    }
}

static void audio_sdl_shutdown(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        if (dev != 0) {
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

struct AudioAPI audio_sdl = {
    audio_sdl_init,
    audio_sdl_buffered,
    audio_sdl_get_desired_buffered,
    audio_sdl_play,
    audio_sdl_shutdown
};
