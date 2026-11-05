/* audio.c — the tune/sfx state machine (game_structure.md §1 subsystem 7)
 * over osys->audio*; the INT 60h ABI is re_map.md's audio section. */
#include "audio.h"
#include "osystem.h"

static const OSystem *_au_osys;
static int32_t _music_active; /** [0x40BB]: the latch music_resume raises   */

/**
 * @brief Bind the audio module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t audio_init(const OSystem *osys)
{
    _au_osys = osys;
    return 0;
}

/** @brief The room frame's gate (1000:A5E8): the MUSIC board's counter [0x5E5B]
 * non-zero -> music_stop (1000:8389: clear the latch, silence the tune);
 * zero -> music_resume (1000:83F7: start it once, latch it).  The backend
 * is the M5 stub, so the only missing piece is the tune handle: the
 * original resumes [0x5AF0]'s tune, the port can only start the driver.
 * The latch gates the calls to the transitions (the original re-sends
 * cmd 6 every frame while a handle is live — no handle here).
 *
 * @param off the offset
 */
void audio_music_gate(uint32_t off)
{
    if (!_au_osys) return; /* the module set without audio_init */
    if (off) {
        if (_music_active) {
            _au_osys->audioStop();
            _music_active = 0;
        }
    } else if (!_music_active) {
        _au_osys->audioStart();
        _music_active = 1;
    }
}
