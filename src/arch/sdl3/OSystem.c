/* The SDL3 backend's vtable and main: binds video/events/timer/audio/fs.
 *
 * Port-only: it implements the OSystem the arch layer owns and runs the
 * core's game_main.  Arguments: the resource directory, --audio, --mt32-roms
 * and --version.
 */
#include "osystem.h"
#include "arch_mem.h"
#include "fs/posix/fs_posix.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#    include <direct.h> // chdir = _chdir on the Windows CRT
/* the macro keeps the CRT's spelling on purpose: it maps the POSIX name */
#    define chdir  _chdir
#    define getcwd _getcwd
#else
#    include <unistd.h>
#endif

#ifdef HUMANSRE_HAVE_MT32
#            define AVAILABLE_AUDIO_DRIVERS  "adlib|beeper|mt32"
#else
#            define AVAILABLE_AUDIO_DRIVERS  "adlib|beeper"
#endif

/** @brief Set the mode, scaling the 320x200 and 320x192 fields to fill 4:3.
 *
 * @param mode the mode
 */
void sdl3_video_set_mode(VideoMode mode);

/**
 * @brief Accessor: one of the two display pages.
 *
 * @param which the page selector
 * @return One of the two display pages.
 */
uint8_t *sdl3_video_page(int32_t which);

/**
 * @brief Present one page.
 *
 * @param which the page selector
 */
void sdl3_video_present(int32_t which);

/**
 * @brief Upload a 6-bit palette range to the display.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
void sdl3_video_set_palette(const uint8_t *p6, uint32_t first, uint32_t n);

/**
 * @brief Clear the input state.
 */
void sdl3_events_init(void);

/**
 * @brief Pump SDL events into the input state.
 *
 * @param out the output buffer
 */
void sdl3_events_poll(InputState *out);

/**
 * @brief Start the fixed-tick timer.
 *
 * @param ticksPerSec the ticks per second
 * @param tick the tick callback
 */
void sdl3_timer_start(uint32_t ticksPerSec, TickFn tick);

/**
 * @brief Block until the next fixed tick has fired.
 *
 * @param lastSeen updated with the counter just run
 */
void sdl3_tick_sync(uint32_t *lastSeen);

/**
 * @brief Install the AdLib driver emulation and open the stream.
 *
 * @return 0 on success, -1 when the driver or the stream cannot be created
 */
int32_t sdl3_audio_install(void);

/**
 * @brief cmd 0x0C: hand the loaded tune image to the driver.
 *
 * @param image the .ALB image
 * @param size its byte length
 * @return 0, always
 */
int32_t sdl3_audio_load_tune(const uint8_t *image, uint32_t size);

/**
 * @brief cmd 5, CX = 0: start the loaded tune's record 0 (music_resume).
 */
void sdl3_audio_resume(void);

/**
 * @brief cmd 6: release the current record (music_stop).
 */
void sdl3_audio_stop(void);

/**
 * @brief cmd 5, CX = id: start a sound-effect record.
 *
 * The original's driver_call stores the new handle in [0x5AF0], overwriting
 * the music handle; that is reproduced here.
 *
 * @param id the event id
 */
void sdl3_audio_sfx(uint16_t id);

/**
 * @brief cmd 0x0A: the master-volume byte.
 *
 * @param cl the game's byte
 */
void sdl3_audio_tempo(uint8_t cl);

/**
 * @brief cmd 2: step the sequencer one tick.
 */
void sdl3_audio_tick(void);

/**
 * @brief Select the resident driver before the install ([0x5AFE]).
 *
 * @param name "adlib" (the default), "beeper" or "mt32"
 * @param roms the MT-32 ROM directory (required for "mt32")
 * @return 0 on success, -1 on an unknown name, -2 when mt32 has no ROM dir
 */
int32_t sdl3_audio_select(const char *name, const char *roms);

/**
 * @brief The backend's [0x5AFE] sound_driver_id (osystem.h).
 *
 * @return 0 for the Beeper, 1 for AdLib, 3 for the MT-32; -1 before a
 * selection is made
 */
int32_t sdl3_audio_driver_id(void);

/**
 * @brief Report a non-fatal diagnostic through SDL's log.
 *
 * @param msg the message
 */
static void _warn(const char *msg);

/**
 * @brief Report a backend failure and exit.
 *
 * @param msg the message
 */
static void _fatal(const char *msg);

const OSystem g_osys_sdl3 = { sdl3_video_set_mode,
                              sdl3_video_page,
                              sdl3_video_present,
                              sdl3_video_set_palette,
                              sdl3_events_poll,
                              sdl3_timer_start,
                              sdl3_tick_sync,
                              sdl3_audio_install,
                              sdl3_audio_load_tune,
                              sdl3_audio_resume,
                              sdl3_audio_stop,
                              sdl3_audio_sfx,
                              sdl3_audio_tempo,
                              sdl3_audio_tick,
                              sdl3_audio_driver_id,
                              _warn,
                              _fatal,
                              &g_fs_posix,
                              arch_mem_alloc,
                              arch_mem_free };

static void _warn(const char *msg)
{ SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", msg); }

static void _fatal(const char *msg)
{
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    exit(1);
}

/* The parsed command line (see main()'s usage). */
typedef struct CliOptions {
    const char *path; // the game-volume directory (mandatory)
    const char *audio; // --audio driver name, NULL = AdLib
    const char *roms; // --mt32-roms directory, NULL = unset
} CliOptions;

/* _parse_args's outcome. */
typedef enum CliParseResult {
    CLI_PARSE_CONTINUE = 0, // a full command line: run the game
    CLI_PARSE_ERROR, // a bad command line, already reported
    CLI_PARSE_VERSION // --version was printed: exit successfully
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
                "USAGE: %s [--audio %s] [--mt32-roms <dir>] "
                "[--version] /path/to/HUMANS/volumes\n",
                argv[0],
                AVAILABLE_AUDIO_DRIVERS);
        return CLI_PARSE_ERROR;
    }
    return CLI_PARSE_CONTINUE;
}

/**
 * @brief Resolve a relative --mt32-roms path against the current directory.
 *
 * main() chdirs into the volume directory before the audio driver loads its
 * ROMs, so the directory has to become absolute while the invoking working
 * directory is still current.
 *
 * @param roms the path from the command line, may be NULL
 * @param buf storage for the absolute form
 * @param size buf's capacity
 * @return roms itself when it is already absolute or cannot be resolved,
 *         otherwise buf
 */
static const char *_roms_absolute(const char *roms, char *buf, size_t size)
{
    char cwd[768];

    if (!roms || !roms[0]) return roms;
    if (roms[0] == '/' || roms[0] == '\\' || roms[1] == ':') return roms;
    if (!getcwd(cwd, (int)sizeof(cwd))) return roms;
    snprintf(buf, size, "%s/%s", cwd, roms);
    return buf;
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
        fprintf(stderr,
            "unknown audio driver '%s' (available: %s)\n",
            opts->audio,
            AVAILABLE_AUDIO_DRIVERS 
        );

        return 1;
    }
}

/**
 * @brief The SDL3 executable: run the core with the SDL backend.
 *
 * Usage: humans-sdl3 [--audio adlib|beeper|mt32] [--mt32-roms <dir>]
 *                    [--version] <path-to-game-resources>
 * The path is chdir'd into so the filesystem module finds the volume files
 * (HUMANS.000/001/002).  --audio selects the resident driver (the launcher's
 * [0x5AFE]); AdLib is the default, `--audio beeper` is the PC speaker,
 * `--audio mt32` is the Roland MT-32 emulation and then --mt32-roms names the
 * directory holding its ROM images (mandatory for mt32).
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    CliOptions opts;
    char roms[1024];

    switch (_parse_args(argc, argv, &opts)) {
    case CLI_PARSE_CONTINUE: break;
    case CLI_PARSE_VERSION: return 0;
    case CLI_PARSE_ERROR: return 1;
    }
    opts.roms = _roms_absolute(opts.roms, roms, sizeof(roms));
    if (_select_audio(&opts) != 0) return 1;
    if (chdir(opts.path) != 0) {
        fprintf(stderr, "cannot chdir to '%s'\n", opts.path);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    sdl3_events_init();

    int32_t r = game_main(&g_osys_sdl3);

    SDL_Quit();
    return r;
}
