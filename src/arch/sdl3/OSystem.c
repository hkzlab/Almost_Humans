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
int32_t sdl3_audio_select(const char *name, const char *roms);
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

/* The arch layer owns main() (port_structure.md §2). */

/* The parsed command line (see main()'s usage block). */
typedef struct CliOptions {
    const char *path; /* the game-volume directory (mandatory) */
    const char *audio; /* --audio driver name, NULL = AdLib */
    const char *roms; /* --mt32-roms directory, NULL = unset */
} CliOptions;

/* _parse_args's outcome. */
typedef enum CliParseResult {
    CLI_PARSE_CONTINUE = 0, /* a full command line: run the game */
    CLI_PARSE_ERROR, /* a bad command line, already reported */
    CLI_PARSE_VERSION /* --version was printed: exit successfully */
} CliParseResult;

/**
 * @brief Parse the command line into opts, printing its own diagnostics.
 *
 * --version prints the banner and asks main to exit; a bad command line
 * prints USAGE.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @param opts the parsed result
 * @return what main should do next
 */
static CliParseResult _parse_args(int argc, char **argv, CliOptions *opts)
{
    int i;

    opts->path = NULL;
    opts->audio = NULL;
    opts->roms = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s %s\n", HUMANSRE_PROJECT_TITLE, HUMANSRE_VERSION);
            return CLI_PARSE_VERSION;
        } else if (strcmp(argv[i], "--audio") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--audio needs a driver name\n");
                return CLI_PARSE_ERROR;
            }
            opts->audio = argv[i];
        } else if (strcmp(argv[i], "--mt32-roms") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--mt32-roms needs a directory\n");
                return CLI_PARSE_ERROR;
            }
            opts->roms = argv[i];
        } else if (!opts->path) {
            opts->path = argv[i];
        } else {
            opts->path = NULL;
            break;
        }
    }
    if (!opts->path) {
        fprintf(stderr,
                "USAGE: %s [--audio adlib|beeper|mt32] [--mt32-roms <dir>] "
                "[--version] /path/to/HUMANS/volumes\n",
                argv[0]);
        return CLI_PARSE_ERROR;
    }
    return CLI_PARSE_CONTINUE;
}

/**
 * @brief Install the driver named on the command line.
 *
 * @param opts the parsed command line
 * @return 0 on success, 1 on an unknown or incomplete selection
 */
static int _select_audio(const CliOptions *opts)
{
    switch (sdl3_audio_select(opts->audio, opts->roms)) {
    case 0: return 0;
    case -2:
        fprintf(stderr, "--audio mt32 needs --mt32-roms <directory>\n");
        return 1;
    default:
#ifdef HUMANSRE_HAVE_MT32
        fprintf(stderr, "unknown audio driver '%s' (adlib, beeper, mt32)\n",
                opts->audio);
#else
        fprintf(stderr,
                "unknown audio driver '%s' (adlib, beeper; this "
                "build has no mt32emu)\n",
                opts->audio);
#endif
        return 1;
    }
}

/* The arch layer owns main() (port_structure.md §2).
 *
 * Usage: humans-sdl3 [--audio adlib|beeper|mt32] [--mt32-roms <dir>]
 *                    [--version] <path-to-game-resources>
 * The path is chdir'd into so the filesystem module finds the volume files
 * (HUMANS.000/001/002).  --audio selects the resident driver (the launcher's
 * [0x5AFE]); AdLib is the default, `--audio beeper` is the PC speaker,
 * `--audio mt32` is the Roland MT-32 emulation and then --mt32-roms names the
 * directory holding its ROM images (mandatory for mt32).  --version prints
 * the port version and exits. */
/**
 * @brief The SDL3 executable: run the core with the SDL backend.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    CliOptions opts;

    switch (_parse_args(argc, argv, &opts)) {
    case CLI_PARSE_CONTINUE: break;
    case CLI_PARSE_VERSION: return 0;
    case CLI_PARSE_ERROR: return 1;
    }
    if (_select_audio(&opts) != 0) return 1;
    if (chdir(opts.path) != 0) {
        fprintf(stderr, "cannot chdir to '%s'\n", opts.path);
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
