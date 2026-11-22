/* The arch ABI: everything the game core takes from the host.
 *
 *   video    vga_set_mode_13h 1000:8300, reset_display_mode 1000:82C6
 *   events   poll_input 1000:8526, int9_keyboard_isr 1000:93A1
 *   timer    int8_timer_isr 1000:8A70
 *   audio    INT 60h, driver_reset 1000:84DE (sound_driver.h)
 *   files    DOS INT 21h; int63_load_resource 1000:8D63 (fs.h)
 *   heap     DOS INT 21h AH=48h; the boot block [0x48AC]
 *            (OSystem.memAlloc/memFree)
 *
 * The core never touches SDL, the GPU, the OS or a path: if a behaviour cannot
 * be expressed here, this interface is wrong, not the core.
 */
#ifndef HUMANSRE_OSYSTEM_H
#define HUMANSRE_OSYSTEM_H

#include "fs/fs.h"

#include <stddef.h>
#include <stdint.h>

/* One poll's input: the two bytes the game's foreground reads, plus the
 * host's quit request.
 *
 *   [0x5ACE]  the control bits.  poll_input (1000:8526) rebuilds them every
 *             frame: read_input_state (1000:85BD) clears the byte and loads
 *             the joystick state when the stick is enabled ([0x5B00]); then
 *             the five bound keys [0x5B05..01] are ORed in.  The port calls
 *             the result `joyBits` and keeps it as a level.
 *   [0x48A3]  the translated key code.  The INT 9 ISR (1000:93A1) writes it
 *             on every key event and 0 on a release, so nonzero means "a key
 *             is held at this instant".  The port splits it into the one-shot
 *             make fields `lastKey`/`keyScan` and the level `keyHeld`.
 *
 * `lastKey`/`keyScan` describe only the make this poll delivers (the last key
 * event wins; a release leaves 0).  `joyBits`, `keyHeld` and `quit` stay set
 * while the condition holds. */
typedef struct InputState {
    /* [0x5ACE] control level: bit0 up, bit1 down, bit2 left, bit3 right,
     * bit7 action/fire.  The arrows and the bound action key (Z in the shipped
     * config) set it; a real stick ORs into the same bits. */
    uint8_t joyBits;

    /* This poll's make as the ISR's translated code: uppercase letters;
     * ESC 0x1B, Enter 0x0D, Backspace 0x0E; 0 when no make is delivered or
     * the make's code is 0 (modifiers, E0-extended keys).  One-shot: the
     * event screens read and clear it (1000:AA4E/1000:AA50). */
    uint8_t lastKey;

    /* PC set-1 scancode of this poll's make, for the keys the backend maps by
     * scancode: ESC 0x01, ',' 0x33, '.' 0x34, SPACE 0x39, F1..F10 0x3B..0x44
     * (the key list at DS:0x1989 binds all but F9); 0 for every other make.
     * The list is matched by scancode (scan_id_list 1000:386E), so lastKey
     * cannot stand in for it.  One-shot. */
    uint8_t keyScan;

    /* [0x48A3]'s level form: 1 while the key down now translates nonzero, 0
     * after any release (1000:942F clears it even if another key is still
     * down).  The readers that test "any key" without consuming make use it
     * (1000:B2BA, 1000:C0C6, attract_exit, cd_file_op). */
    uint8_t keyHeld;

    /* Host quit request (window close, ^C, --ticks); sticky, not a game
     * input; the flow loop reads input.quit. */
    uint8_t quit;
} InputState;

#define JOYBITS_UP    (1u << 0)
#define JOYBITS_DOWN  (1u << 1)
#define JOYBITS_LEFT  (1u << 2)
#define JOYBITS_RIGHT (1u << 3)
#define JOYBITS_DIRS  (JOYBITS_UP | JOYBITS_DOWN | JOYBITS_LEFT | JOYBITS_RIGHT)
#define JOYBITS_FIRE  (1u << 7)

/* The fixed-tick callback: everything the original's INT 8 ISR body did, in
 * the same order (the tick counter, the palette-fade step, the pending page
 * flip apply) happens here, once per tick. */
typedef void (*TickFn)(void);

/* The two VGA modes the game sets; the core keeps its 320x200x8 pages and
 * only says which field the CRTC would scan out, so the backend can present
 * that field (and nothing past it as the mode-X pages carry stale rows below
 * the 192nd):
 *   VIDEO_MODE_13H  320x200 -> BIOS mode 13h (1000:88FB) and the attract's
 *                   vga_set_mode_13h (1000:8300): CRTC 0x12 = 0x8F,
 *                   0x13 = 0x28.
 *   VIDEO_MODE_X    320x192 -> reset_display_mode (1000:82C6), the room /
 *                   party / level field: CRTC 0x12 = 0x7F, 0x13 = 0x2C
 *                   (the level's 40-row status bar ends on row 191, and
 *                   the original's raw frames of those screens are 320x192).
 */
typedef enum VideoMode {
    VIDEO_MODE_13H = 0,
    VIDEO_MODE_X = 1
} VideoMode;

/* The viewport the front end asked for.  Distinct from VideoMode, which is
 * the CRTC's scan-out mode: VIEWPORT_CLASSIC is the original's 320x200
 * framing, VIEWPORT_WIDE the doubled level view (D-274). */
typedef enum ViewportMode {
    VIEWPORT_CLASSIC = 0,
    VIEWPORT_WIDE = 1
} ViewportMode;

/* The launcher's [0x5AFE] sound_driver_id.  These are the values the original
 * tests: play_tune picks .BEP/.ALB for 0/1 and .RLD for anything else
 * (1000:8462), and driver_call sends sound effect 0x0E only for 3
 * (1000:83D3).  An id outside this enum is a backend fault: the core warns
 * and silences the music (D-280). */
typedef enum AudioDriverId {
    AUDIO_DRIVER_NONE = -1, // [0x5AFE] = 0xFFFF: no driver loaded
    AUDIO_DRIVER_BEEPER = 0,
    AUDIO_DRIVER_ADLIB = 1,
    AUDIO_DRIVER_MT32 = 3
} AudioDriverId;

/* The original's screen: flat 8bpp, one byte per pixel, 320x200.  Every
 * resource that is one screen page is this size and every screen module
 * draws on a surface this size.  The viewport mode never changes it. */
#define VIDEO_SCREEN_W     320u
#define VIDEO_SCREEN_H     200u
#define VIDEO_SCREEN_BYTES (VIDEO_SCREEN_W * VIDEO_SCREEN_H)
/* The level's field in mode X: the original scans out 192 of the 200 rows. */
#define VIDEO_FIELD_H_MODEX 192u

/* The wide viewport compiled in: 1 includes the doubled view (the `--view
 * wide` runtime switch, D-274), 0 compiles it out - the classic framing only,
 * with 320x200 pages and none of the wide code.  The build sets it (CMake's
 * HUMANSRE_VIEWPORT_WIDE); 1 is the default for a standalone TU. */
#ifndef HUMANSRE_VIEWPORT_WIDE
#    define HUMANSRE_VIEWPORT_WIDE 1
#endif

/* The display page videoPage() hands out, allocated at the compiled maximum
 * (VIDEO_SCREEN_W/H x the 2x wide factor, or 1 when compiled out).  The
 * active size is video_page_w()/h(); the field a mode scans out may be
 * shorter (mode X).  Stride == the active width because the page is 8bpp. */
#define VIDEO_PAGE_MAX_W (VIDEO_SCREEN_W * (HUMANSRE_VIEWPORT_WIDE + 1))
#define VIDEO_PAGE_MAX_H (VIDEO_SCREEN_H * (HUMANSRE_VIEWPORT_WIDE + 1))
#define VIDEO_PAGE_BYTES (VIDEO_PAGE_MAX_W * VIDEO_PAGE_MAX_H)

/* The tick-boundary gate, the core's only pacing source.
 *
 * The arch owns the tick counter (the port's [0x5AC8]) and advances it once
 * per tick at the point the original ISR does (1000:8AA4, before the tick
 * body's later work).  *lastSeen is in/out: tickSync blocks until the
 * counter differs from *lastSeen, then writes the observed count back.  Any
 * difference counts, not only an increase (the counter wraps), and a tick
 * that already landed since the previous call returns at once.
 *
 * This is the foreground's snapshot-and-compare spin on [0x5AC8],
 * not the [0x48BA] vsync-abort flag and not a display wait:
 * the host display refresh must never pace the game.
 * timerStart installs the tick body; tickSync only observes its
 * counter, which may be advanced on another thread, so the gate must be
 * safe to call from the foreground.
 * 
 * Call it once per foreground iteration, before that iteration's
 * flow step - one flow step per tick boundary. */
typedef void (*TickSyncFn)(uint32_t *lastSeen);

typedef struct OSystem {
    /* --- video ------------------------------------------------------- */
    void (*videoSetMode)(VideoMode mode); // every mode set (13h/X)
    uint8_t *(*videoPage)(int32_t which); // which: 0 or 1; the active page
    /* The CRTC-flip equivalent: show `which` as a `rows`-row field.  `rows`
     * is the current screen's scan-out height (the original's 192 for mode X
     * and 200 for mode 13h, scaled by the view factor; the wide level's own
     * frame is its whole page - see video_set_field).  The backend sizes its
     * texture from it and rebuilds it only when the value changes. */
    void (*videoPresent)(int32_t which, uint32_t rows);
    void (*videoSetPalette)(const uint8_t *p6, uint32_t first, uint32_t n);
    /* The viewport the front end was asked for: VIEWPORT_CLASSIC is the
     * original's 320x200 framing, VIEWPORT_WIDE doubles the level viewport.
     * Read once by video_init() and clamped to HUMANSRE_VIEWPORT_WIDE; the
     * core never switches it afterwards.  The backend owns where the value
     * comes from (command line, config, the build's default). */
    ViewportMode (*viewportMode)(void);

    /* --- input ------------------------------------------------------- */
    void (*eventsPoll)(InputState *out);

    /* --- timing ------------------------------------------------------ */
    void (*timerStart)(uint32_t ticksPerSec, TickFn tick);
    TickSyncFn tickSync; // the [0x5AC8] counter gate, not [0x48BA]

    /* --- audio ------------------------------------------------------- *
     * One member per INT 60h command the game sends.  The module behind them
     * is the resident driver's replacement; the backend owns the sample
     * stream and pulls the module's generate().                           */
    int32_t (*audioInstall)(void); // the install entry (1E95:00DD)
    int32_t (*audioLoadTune)(const uint8_t *image, uint32_t size); // 0x0C
    void (*audioResume)(void); // cmd 5, CX = 0 (music_resume)
    void (*audioStop)(void); // cmd 6 (music_stop)
    void (*audioSfx)(uint16_t id); // cmd 5, CX = the event id
    void (*audioTempo)(uint8_t cl); // cmd 0x0A (master volume)
    void (*audioTick)(void); // cmd 2 (the music tick)
    /* [0x5AFE] sound_driver_id.  play_tune chooses the tune extension and
     * driver_call its gate from it (1000:83CC, 1000:8462), exactly as the
     * original's launcher global. */
    AudioDriverId (*audioDriverId)(void);

    /* --- misc -------------------------------------------------------- */
    /* A non-fatal diagnostic: the core formats the message, the backend owns
     * the destination (SDL log, stderr, nothing).  Not a DOS behaviour - the
     * original had no logging - but the core must not know a stream either. */
    void (*warn)(const char *msg);
    void (*fatalError)(const char *msg); // does not return

    /* The filesystem module the core reads the volumes and saves through
     * It is defined by the arch layer. */
    const FSModule *fs;

    /* --- heap -------------------------------------------------------- *
     * The original's DOS allocation (INT 21h AH=48h): the one way the core
     * obtains a dynamic buffer.  Callers go through the active backend
     * (g_osys->memAlloc/memFree); no core module calls malloc/free. */
    void *(*memAlloc)(size_t size); // a block, or NULL when unavailable
    void (*memFree)(void *p); // release a memAlloc block
} OSystem;

/* The one backend the core talks to.  osys_bind() writes it once at boot
 * (game_main, first statement), before the first core allocation or vtable
 * call; it is read-only afterwards, so the timer and audio threads start only
 * after the bind.  The vtable, including memAlloc/memFree, is always fully populated,
 * so core code needs no unbound test before using it. */
extern const OSystem *g_osys;

/**
 * @brief Bind the one backend the core talks to.
 *
 * Called once by game_main at boot, before the init pass.  A test that drives
 * modules outside game_main calls it before the init pass it exercises.
 *
 * @param osys the backend interface (fully populated)
 */
void osys_bind(const OSystem *osys);

#endif /* HUMANSRE_OSYSTEM_H */
