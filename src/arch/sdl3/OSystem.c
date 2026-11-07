/* arch/sdl3/OSystem.c — the SDL3 vtable + main (port_structure.md §2). */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_oldnames.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#    include <direct.h> /* chdir = _chdir on the Windows CRT */
/* the macro keeps the CRT's spelling on purpose: it maps the POSIX name */
#    define chdir _chdir
#else
#    include <unistd.h>
#endif

void sdl3_video_set_mode(VideoMode mode);
uint8_t *sdl3_video_page(int32_t which);
void sdl3_video_present(int32_t which);
void sdl3_video_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);
void sdl3_events_init(void);
void sdl3_events_poll(InputState *out);
void sdl3_timer_start(uint32_t ticksPerSec, TickFn tick);
void sdl3_tick_sync(uint32_t *lastSeen);
int32_t sdl3_audio_install(void);
int32_t sdl3_audio_load_tune(const uint8_t *image, uint32_t size);
void sdl3_audio_resume(void);
void sdl3_audio_stop(void);
void sdl3_audio_sfx(uint16_t id);
void sdl3_audio_tempo(uint8_t cl);
void sdl3_audio_tick(void);
int32_t sdl3_audio_select(const char *name);
int32_t sdl3_audio_driver_id(void);
static void _fatal(const char *msg);

const OSystem g_osys_sdl3 = { sdl3_video_set_mode,  sdl3_video_page,
                              sdl3_video_present,   sdl3_video_set_palette,
                              sdl3_events_poll,     sdl3_timer_start,
                              sdl3_tick_sync,       sdl3_audio_install,
                              sdl3_audio_load_tune, sdl3_audio_resume,
                              sdl3_audio_stop,      sdl3_audio_sfx,
                              sdl3_audio_tempo,     sdl3_audio_tick,
                              sdl3_audio_driver_id, _fatal };

/**
 * @brief Report a backend failure and exit.
 *
 * @param msg the message
 */
static void _fatal(const char *msg)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    exit(1);
}

/* The arch layer owns main() (port_structure.md §2).
 *
 * Usage: humans-sdl3 [--audio adlib|beeper] <path-to-game-resources>
 * The path is chdir'd into so the filesystem module finds the volume files
 * (HUMANS.000/001/002); without it the current directory is used.  --audio
 * selects the resident driver (the launcher's [0x5AFE]); AdLib is the
 * default, `--audio beeper` is the PC speaker. */
/**
 * @brief The SDL3 executable: run the core with the SDL backend.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *audio = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--audio") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--audio needs a driver name\n");
                return 1;
            }
            audio = argv[i];
        } else if (!path) {
            path = argv[i];
        } else {
            path = NULL;
            break;
        }
    }
    if (!path) {
        fprintf(stderr,
                "USAGE: %s [--audio adlib|beeper] "
                "/path/to/HUMANS/volumes\n",
                argv[0]);
        return 1;
    }
    if (sdl3_audio_select(audio) != 0) {
        fprintf(stderr, "unknown audio driver '%s' (adlib, beeper)\n", audio);
        return 1;
    }
    if (chdir(path) != 0) {
        fprintf(stderr, "cannot chdir to '%s'\n", path);
        return 1;
    }

    // Initialize video mode
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Initialize event structures
    sdl3_events_init();

    // Hand over to game code
    int32_t r = game_main(&g_osys_sdl3);

    // Time to go
    SDL_Quit();
    return r;
}
