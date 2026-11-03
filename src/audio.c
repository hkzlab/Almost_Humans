/* audio.c — the tune/sfx state machine (game_structure.md §1 subsystem 7)
 * over osys->audio*; the INT 60h ABI is re_map.md's audio section. */
#include "game.h"

static const OSystem *au_osys;
static int32_t musicActive;  /* [0x40BB]: the latch music_resume raises   */

int32_t audio_init(const OSystem *osys) { au_osys = osys; return 0; }

/* The room frame's gate (1000:A5E8): the MUSIC board's counter [0x5E5B]
 * non-zero -> music_stop (1000:8389: clear the latch, silence the tune);
 * zero -> music_resume (1000:83F7: start it once, latch it).  The backend
 * is the M5 stub, so the only missing piece is the tune handle: the
 * original resumes [0x5AF0]'s tune, the port can only start the driver.
 * The latch gates the calls to the transitions (the original re-sends
 * cmd 6 every frame while a handle is live — no handle here). */
void audio_musicGate(uint32_t off)
{
    if (!au_osys) return;          /* the module set without audio_init */
    if (off) {
        if (musicActive) {
            au_osys->audioStop();
            musicActive = 0;
        }
    } else if (!musicActive) {
        au_osys->audioStart();
        musicActive = 1;
    }
}
