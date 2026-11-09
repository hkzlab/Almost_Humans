/* arch/audio/mt32/sound_driver_mt32.c — MT32.DRV's replacement over mt32emu.
 *
 * MT32.DRV is 5600 bytes of position-independent 8086 code (a CD resource,
 * loaded into a DOS-allocated segment; every address below is an offset into
 * the shipped image).  This file is that image's behaviour, read from the
 * Ghidra listing on 2026-11-07; the tune byte layout is rld_format.md, the
 * sequencer model its §9 and the routine table its §13.
 *
 *   INT 60h            MT32.DRV                     here
 *   install AX=0x60    0x00DE                       _mt32_create
 *   cmd 0              0x0178 (uninstall/reset)     _mt32_destroy
 *   cmd 2              0x0792 -> 0x07A0             _mt32_tick
 *   cmd 5              0x02A7                       _mt32_start_event
 *   cmd 6              0x03FA                       _mt32_release_event
 *   cmd 0x0A           0x01CD (master volume)       _mt32_set_tempo
 *   cmd 0x0C           0x0236 (load tune)           _mt32_load_tune
 *   cmd 0x0E           0x01BB (identity block)      _mt32_command_block
 *
 * cmd 0x0A is a master *volume*, not a tempo: CL is clamped to 100 and sent
 * as the system-area DT1 10 00 16 (<volume>); the game fades with it.  The
 * driver never installs its own INT 8 through the game (cmd 3 is unused), so
 * cmd 2 is the only clock, exactly as in the OSystem ABI.
 *
 * MIDI goes to mt32emu through the byte-stream parser (mt32emu_parse_stream),
 * so every byte the driver would put on the MPU-401 data port is the byte the
 * emulator sees — running status included.  The emulator library is a system
 * dependency; the ROM images are read here and handed over with
 * mt32emu_add_rom_data() (never mt32emu_add_rom_file), and the buffers stay
 * alive for the context's lifetime.  A multi-model ROM directory resolves to
 * the newest MT-32 model present (control ROM 2.07 first, then 2.06, 2.04,
 * 2.03, 1.07, 1.06, 1.05, 1.04, and only then CM-32L), and both emulator
 * output gains (dry and reverb) sit at 0.501 — DOSBox Staging's default
 * -6 dB mixer master gain — so the port's level matches DOSBox's.
 *
 * Tune image (rld_format.md §1):
 *   header    0x000..0x117 (new format), order list +0x18, instruments +0x98
 *   records   rec_base + id*16, 128 slots, 8 runtime records
 *   offsets   off_base .. +0x200, 256 le16 paragraphs
 *   patterns  off_base + T[order]*16, 64 rows each
 *   timbres   off_base + T[P]*16 + 0x100 per flagged instrument
 *   rec_base = off_base - 0x800 (0x158 new / 0x118 old)
 *
 * Deliberately not emulated: cmd 7's master-volume envelope (the game never
 * sends cmd 7, so the envelope stays disarmed; cmd 8 only reads its flag),
 * cmd 1/3/4/9/0x0B/0x0D and the MPU-401 handshake bytes, which have no
 * meaning without a physical interface.  The install's *synth* side is sent
 * (`_install_reset`: the MT-32 all-parameters reset, the channel CC/bend
 * reset and the reverb-off system block), so the emulated unit starts in the
 * state MT32.DRV's install leaves it in.
 */
#include "audio/sound_driver.h"

#include "audio/mt32/sound_driver_mt32.h"
#include "byteorder.h"

#include <mt32emu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dirent.h>
#endif

#define MT32_RECORDS  8u
#define MT32_SLOTS    8u
#define MT32_ROWS     0x40u
#define MT32_HEADER   0x158u
#define MT32_OFF_NEW  0x958u
#define MT32_OFF_OLD  0x918u
#define MT32_ROMS_MAX 64u
/* mt32emu's output gain is 1.0 by default; 0.501 is DOSBox Staging's default
 * -6 dB mixer master gain, so the port's absolute level matches DOSBox's. */
#define MT32_OUTPUT_GAIN 0.501f


/* The ROM pair to prefer, newest MT-32 model first.  `mt32emu_open_synth`
 * picks the emulated machine from the control ROM, so a directory holding
 * several models (the MAME-versioned sets) must be chosen explicitly instead
 * of letting the directory order decide.  The ids are mt32emu's own
 * `control_rom_id`/`pcm_rom_id` (mt32emu_get_rom_ids). */
static const char *_rom_preference[][2] = {
    { "ctrl_mt32_2_07", "pcm_mt32" }, /* MT-32 "new", latest revision */
    { "ctrl_mt32_2_06", "pcm_mt32" },
    { "ctrl_mt32_2_04", "pcm_mt32" },
    { "ctrl_mt32_2_03", "pcm_mt32" },
    { "ctrl_mt32_1_07", "pcm_mt32" }, /* MT-32 "old", latest revision */
    { "ctrl_mt32_1_06", "pcm_mt32" },
    { "ctrl_mt32_1_05", "pcm_mt32" },
    { "ctrl_mt32_1_04", "pcm_mt32" },
    { "ctrl_mt32_bluer", "pcm_mt32" },
    { "ctrl_cm32l_1_02", "pcm_cm32l" }, /* only when no MT-32 ROM is here */
    { "ctrl_cm32l_1_00", "pcm_cm32l" },
    { "ctrl_cm32ln_1_00", "pcm_cm32l" }
};

/* CS:0x12A0, the semitone period table 0x12A0..0x1377 (108 words); only
 * mt32_slide_setup (0x0BCD) reads it. */
static const uint16_t _period[108] = {
    6848, 6464, 6096, 5760, 5424, 5120, 4832, 4560, 4304, 4064, 3840, 3616,
    3424, 3232, 3048, 2880, 2712, 2560, 2416, 2280, 2152, 2032, 1920, 1808,
    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960,  904,
    856,  808,  762,  720,  678,  640,  604,  570,  538,  508,  480,  452,
    428,  404,  381,  360,  339,  320,  302,  285,  269,  254,  240,  226,
    214,  202,  190,  180,  169,  160,  151,  142,  134,  127,  120,  113,
    107,  101,  95,   90,   84,   80,   75,   71,   67,   63,   60,   56,
    53,   50,   47,   45,   42,   40,   37,   35,   33,   31,   30,   28,
    26,   25,   23,   22,   21,   20,   18,   17,   16,   15,   15,   14
};

/* Every byte of the driver's 32-byte MIDI slot (CS:0x1401 + slot*0x20). */
typedef struct Mt32Slot {
    uint8_t note[4];      /* +0x00 four chord notes, 0 = none      */
    uint8_t instrument;   /* +0x04 1-based, 0 = no change          */
    uint8_t command;      /* +0x05 table index (cmd & 0x1F)        */
    uint8_t param;        /* +0x06                                 */
    uint8_t volume;       /* +0x07  0..0x40                        */
    uint16_t bend;        /* +0x08  centre 0x2000                  */
    uint8_t last_note;    /* +0x0A                                 */
    uint8_t last_program; /* +0x0B  0xFF = nothing sent yet        */
    uint16_t slide_target;/* +0x0C  period, 0 = no slide           */
    uint8_t slide_dir;    /* +0x0E  0 down, 1 up                    */
    uint8_t slide_step;   /* +0x0F                                */
    uint16_t slide_acc;   /* +0x10                                */
    uint8_t slide_note;   /* +0x12  the note the slide lands on    */
} Mt32Slot;

/* The driver's 22-byte record (CS:0x1501 + id*0x16). */
typedef struct Mt32Record {
    uint8_t active;              /* +0x00 0xFF while playing           */
    uint8_t tracks;              /* +0x01                              */
    uint8_t ticks_per_row;       /* +0x02 default 6                    */
    uint8_t order;               /* +0x03 index into the order list    */
    uint16_t cursor;             /* +0x04 pattern-relative byte offset */
    uint8_t tick;                /* +0x06                              */
    uint8_t advance;             /* +0x07 NOT toggled by cmds 0x0B/0x0D */
    uint8_t row;                 /* +0x08 rows decoded, 0x40 = end     */
    uint8_t loop;                /* +0x09 file +0x0C                   */
    uint8_t first_order;         /* +0x0A                              */
    uint8_t last_order;          /* +0x0B file +0x0B + 1               */
    uint8_t finished;            /* +0x0C                              */
    uint8_t track_slot[MT32_SLOTS]; /* +0x0D free-list slot numbers    */
} Mt32Record;

struct SoundDriver {
    uint32_t sampleRate;
    mt32emu_context ctx;
    uint8_t ready;
    uint8_t volume;                   /* CS:0x108E, initial 0x4B        */
    uint8_t track_volume[16];         /* CS:0x1070, indexed slot+1      */
    uint8_t command_block[8];         /* CS:0x15D1, cmd 0x0E's buffer   */
    /* CS:0x0EAA..0x0EAC: the DT1 data command 7/8 writes and sends.  It
     * persists between messages, so command 8 keeps the mode command 7
     * left in byte 0 (the image starts 00 04 04). */
    uint8_t system_block[3];
    Mt32Slot slot[MT32_SLOTS];
    Mt32Record record[MT32_RECORDS];
    uint8_t slot_free[MT32_SLOTS];    /* CS:0x15C1                      */
    uint8_t *tune;
    uint32_t tune_size;
    uint32_t off_base;                /* CS:0x13FD: 0x958 / 0x918       */
    uint32_t record_base;             /* off_base - 0x800               */
    uint32_t bank_base;               /* off_base + T[P]*16             */
    uint8_t order_list[0x7F];         /* file +0x18                     */
    uint8_t instruments[0x80];        /* file +0x98                     */
    uint8_t trailing;                 /* file +0x118                    */
    uint16_t offset[256];             /* CS:0x1098                      */
    uint16_t offset_count;
    /* The mt32emu ROM buffers: mt32emu_add_rom_data does not copy, so they
     * must outlive the context.  `rom_control`/`rom_pcm` are the ids
     * mt32emu_identify_rom_data assigned to them. */
    uint8_t *rom[MT32_ROMS_MAX];
    size_t rom_size[MT32_ROMS_MAX];
    const char *rom_control[MT32_ROMS_MAX];
    const char *rom_pcm[MT32_ROMS_MAX];
    unsigned rom_count;
};

static char _rom_dir[1024];

void sound_driver_mt32_set_roms(const char *path)
{
    if (!path) {
        _rom_dir[0] = '\0';
        return;
    }
    snprintf(_rom_dir, sizeof(_rom_dir), "%s", path);
}

/**
 * @brief One MIDI data byte from the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the byte
 */
static uint8_t _u8(const SoundDriver *drv, uint32_t off)
{
    if (off >= drv->tune_size) return 0;
    return drv->tune[off];
}

/**
 * @brief One little-endian word from the tune image, or 0 past its end.
 *
 * @param drv the driver
 * @param off the byte offset into the tune
 * @return the word
 */
static uint16_t _u16(const SoundDriver *drv, uint32_t off)
{
    if (off + 2u > drv->tune_size) return 0;
    return rd_le_u16(drv->tune + off);
}

/**
 * @brief Emit a complete short MIDI message (mt32_send_byte's payload).
 *
 * @param drv the driver
 * @param status the status byte (0x90/0xB0/0xC0/0xE0 + channel)
 * @param d1 the first data byte
 * @param d2 the second data byte
 */
static void _midi(SoundDriver *drv, uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t m[3];

    m[0] = status;
    m[1] = (uint8_t)(d1 & 0x7Fu);
    m[2] = (uint8_t)(d2 & 0x7Fu);
    mt32emu_parse_stream(drv->ctx, m, 3);
}

/**
 * @brief Emit one program change — the driver's only 2-byte message.
 *
 * @param drv the driver
 * @param channel the part (1..8)
 * @param program the 0-based program
 */
static void _program(SoundDriver *drv, uint8_t channel, uint8_t program)
{
    uint8_t m[2];

    m[0] = (uint8_t)(0xC0u + channel);
    m[1] = (uint8_t)(program & 0x7Fu);
    mt32emu_parse_stream(drv->ctx, m, 2);
}

/**
 * @brief Send one Roland DT1 sysex the way mt32_send_block (0x0D54) does.
 *
 * @param drv the driver
 * @param a0 the address high byte
 * @param a1 the address middle byte
 * @param a2 the address low byte
 * @param data the data bytes
 * @param n the data byte count
 */
static void _sysex(SoundDriver *drv, uint8_t a0, uint8_t a1, uint8_t a2,
                   const uint8_t *data, uint32_t n)
{
    uint8_t buf[8u + 0x3Au + 2u];
    uint32_t i;
    uint8_t sum = 0;

    buf[0] = 0xF0u;
    buf[1] = 0x41u; /* Roland */
    buf[2] = 0x10u; /* unit 0x10 */
    buf[3] = 0x16u; /* MT-32 model */
    buf[4] = 0x12u; /* DT1 */
    buf[5] = a0;
    buf[6] = a1;
    buf[7] = a2;
    sum = (uint8_t)(sum + a0 + a1 + a2);
    for (i = 0; i < n; i++) buf[8u + i] = data[i];
    for (i = 0; i < n; i++) sum = (uint8_t)(sum + data[i]);
    buf[8u + n] = (uint8_t)((0u - (unsigned)sum) & 0x7Fu);
    buf[8u + n + 1u] = 0xF7u;
    mt32emu_parse_stream(drv->ctx, buf, n + 10u);
}

/**
 * @brief Send the master volume (mt32_send_master_volume, 0x058A).
 *
 * @param drv the driver
 */
static void _send_master_volume(SoundDriver *drv)
{
    uint8_t data = drv->volume;
    _sysex(drv, 0x10u, 0x00u, 0x16u, &data, 1u);
}

/**
 * @brief CC 0x7B / 0x79 / 7 / 0x0A on one part (0x05F1).
 *
 * @param drv the driver
 * @param channel the part (1..8)
 */
static void _all_notes_off(SoundDriver *drv, unsigned channel)
{
    uint8_t status = (uint8_t)(0xB0u + channel);

    _midi(drv, status, 0x7Bu, 0x00u);
    _midi(drv, status, 0x79u, 0x00u);
    _midi(drv, status, 0x07u, 0x7Fu);
    _midi(drv, status, 0x0Au, 0x40u);
}

/**
 * @brief Send the 14-bit pitch bend (0x0A94).
 *
 * @param drv the driver
 * @param channel the part (1..8)
 * @param slot the slot holding the bend
 */
static void _send_bend(SoundDriver *drv, unsigned channel, const Mt32Slot *slot)
{
    _midi(drv, (uint8_t)(0xE0u + channel), (uint8_t)(slot->bend & 0x7Fu),
          (uint8_t)((slot->bend >> 7) & 0x7Fu));
}

/**
 * @brief The channel status a slot uses: the original's BP = slot + 1.
 *
 * @param slot the slot
 * @return the MIDI channel number (1..8)
 */
static unsigned _channel(unsigned slot) { return slot + 1u; }

/**
 * @brief Note on with the driver's preceding all-notes-off (0x0B7C).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param note the note (0 = skip)
 */
static void _note_on(SoundDriver *drv, unsigned slot_no, uint8_t note)
{
    Mt32Slot *s = &drv->slot[slot_no];
    unsigned ch = _channel(slot_no);

    if (note == 0) return;
    if (s->bend != 0x2000u) {
        s->bend = 0x2000u;
        _send_bend(drv, ch, s);
    }
    _midi(drv, (uint8_t)(0xB0u + ch), 0x7Bu, 0x00u);
    s->last_note = note;
    _midi(drv, (uint8_t)(0x90u + ch), note, 0x7Fu);
}

/**
 * @brief One additional chord note (0x0BB7).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param note the note (0 = skip)
 */
static void _note_on_extra(SoundDriver *drv, unsigned slot_no, uint8_t note)
{
    if (note == 0) return;
    _midi(drv, (uint8_t)(0x90u + _channel(slot_no)), note, 0x7Fu);
}

/**
 * @brief The period-table bias for a note (0x0BCD).
 *
 * @param note the note number
 * @return the period; out-of-range reads the last entry
 */
static uint16_t _period_of(uint8_t note)
{
    if (note >= 108u) return _period[107];
    return _period[note];
}

/**
 * @brief Note command 3 (0x0BCD): set up a pitch-bend slide.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _slide_setup(SoundDriver *drv, unsigned slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint16_t target = _period_of(s->note[0]);
    uint16_t current;

    s->slide_note = s->note[0];
    s->slide_target = target;
    s->slide_acc = _period_of(s->last_note);
    current = s->slide_acc;
    s->slide_dir = 0;
    if (target == current) {
        s->slide_target = 0;
        return;
    }
    if (target > current) s->slide_dir = 1;
}

/**
 * @brief Tick command 3 (0x0C05): step the slide by one tick's worth.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _slide_tick(SoundDriver *drv, unsigned slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint16_t step;

    if (s->param != 0) {
        s->slide_step = s->param;
        s->param = 0;
    }
    if (s->slide_target == 0) return;
    step = s->slide_step;
    if (s->slide_dir == 0) {
        s->slide_acc = (uint16_t)(s->slide_acc + step);
        s->bend = (uint16_t)(s->bend - (uint16_t)(step << 5));
        if (s->slide_acc < s->slide_target) {
            _send_bend(drv, _channel(slot_no), s);
            return;
        }
    } else {
        uint16_t before = s->slide_acc;
        s->slide_acc = (uint16_t)(s->slide_acc - step);
        if (s->slide_acc > before) { /* borrow: the accumulator hit zero */
            s->slide_target = 0;
            _note_on(drv, slot_no, s->slide_note);
            return;
        }
        s->bend = (uint16_t)(s->bend + (uint16_t)(step << 5));
        if (s->slide_acc > s->slide_target) {
            _send_bend(drv, _channel(slot_no), s);
            return;
        }
    }
    s->slide_target = 0;
    _note_on(drv, slot_no, s->slide_note);
}

/**
 * @brief The record a note command acts on (the original's CS:0x13FF).
 *
 * @param drv the driver
 * @param rec the record being played
 */
typedef struct Mt32Play {
    SoundDriver *drv;
    Mt32Record *rec;
} Mt32Play;

/**
 * @brief The CC 7 value for a 0..0x40 volume: double it, clamp at 0x7F.
 *
 * @param volume the slot volume
 * @return the 7-bit controller value
 */
static uint8_t _cc_volume(uint8_t volume)
{
    unsigned value = (unsigned)volume << 1;
    return (uint8_t)(value > 0x7Fu ? 0x7Fu : value);
}

/**
 * @brief The per-tick commands, table CS:0x1378 (0x0A02).
 *
 *  00 0x0A28 RET          01 0x0A82 bend up by param<<5
 *  02 0x0AAE bend down    03 0x0C05 slide tick
 *  09 0x0A29 volume delta (high nibble adds, low nibble subtracts)
 *  every other index 0x0A27 RET
 *
 * @param p the play state
 * @param slot_no the slot
 */
static void _tick_command(Mt32Play *p, unsigned slot_no)
{
    SoundDriver *drv = p->drv;
    Mt32Slot *s = &drv->slot[slot_no];

    if ((uint8_t)(s->command | s->param) == 0) return;
    switch (s->command & 0x1Fu) {
    case 0x01: /* 0x0A82: bend += param * 0x20, then send */
        s->bend = (uint16_t)(s->bend + (uint16_t)(s->param << 5));
        _send_bend(drv, _channel(slot_no), s);
        break;
    case 0x02: /* 0x0AAE: bend -= param * 0x20, then send */
        s->bend = (uint16_t)(s->bend - (uint16_t)(s->param << 5));
        _send_bend(drv, _channel(slot_no), s);
        break;
    case 0x03: /* 0x0C05: slide */ _slide_tick(drv, slot_no); break;
    case 0x09: { /* 0x0A29: volume delta, then CC 7 = volume * 2 */
        uint8_t high = (uint8_t)(s->param >> 4);
        uint8_t low = (uint8_t)(s->param & 0x0Fu);
        unsigned ch = _channel(slot_no);
        uint8_t value;
        if (high != 0) {
            unsigned next = (unsigned)s->volume + high;
            s->volume = (uint8_t)(next > 0x40u ? 0x40u : next);
        } else if (low > s->volume) {
            s->volume = 0;
        } else {
            s->volume = (uint8_t)(s->volume - low);
        }
        value = s->volume;
        if (drv->track_volume[ch] == value) return;
        drv->track_volume[ch] = value;
        _midi(drv, (uint8_t)(0xB0u + ch), 0x07u, _cc_volume(value));
        break;
    }
    default: break;
    }
}

/**
 * @brief Note command 6 (0x0AC2): all notes off on the slot's part.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _cmd_all_notes_off(SoundDriver *drv, unsigned slot_no)
{
    drv->slot[slot_no].command = 0;
    _midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x7Bu, 0x00u);
}

/**
 * @brief Note command 5 (0x0B04): pan, CC 0x0A.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _cmd_pan(SoundDriver *drv, unsigned slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint8_t panic = s->param;
    uint8_t value;

    if ((panic & 0xF0u) != 0) {
        value = (uint8_t)(((panic & 0xF0u) >> 2) + 0x40u);
    } else if ((panic & 0x0Fu) != 0) {
        value = (uint8_t)(0x40u - (uint8_t)((panic & 0x0Fu) << 2));
    } else {
        s->command = 0;
        _midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x0Au, 0x40u);
        return;
    }
    _midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x0Au, value);
}

/**
 * @brief Note command 4 (0x0C70): modulation, CC 1.
 *
 * The value is (cell note 4 & 0x0F) << 3 — the original reads SI+3 (the
 * fourth chord note), not the parameter byte.
 *
 * @param drv the driver
 * @param slot_no the slot
 */
static void _cmd_modulation(SoundDriver *drv, unsigned slot_no)
{
    uint8_t value = (uint8_t)((drv->slot[slot_no].note[3] & 0x0Fu) << 3);
    _midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x01u, value);
}

/**
 * @brief Note commands 7/8 (0x0AD7/0x0AE3): the 10 00 01 system word.
 *
 * The data bytes live in the driver's CS:0x0EAA block and are not rebuilt:
 * command 7 writes the mode byte only, command 8 the two reverb nibbles only,
 * so the mode set by command 7 survives into command 8's message.
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param nibbles true for command 8's two nibbles, false for command 7
 */
static void _cmd_sysex_system(SoundDriver *drv, unsigned slot_no, int nibbles)
{
    Mt32Slot *s = &drv->slot[slot_no];

    s->command = 0;
    if (nibbles) {
        drv->system_block[1] = (uint8_t)(s->param & 0x0Fu);
        drv->system_block[2] = (uint8_t)(s->param >> 4);
    } else {
        drv->system_block[0] = s->param;
    }
    _sysex(drv, 0x10u, 0x00u, 0x01u, drv->system_block, 3u);
}

/**
 * @brief Note commands, table CS:0x13B8 (0x0A19).
 *
 * @param p the play state
 * @param slot_no the slot
 */
static void _note_command(Mt32Play *p, unsigned slot_no)
{
    SoundDriver *drv = p->drv;
    Mt32Slot *s = &drv->slot[slot_no];
    Mt32Record *rec = p->rec;

    switch (s->command & 0x1Fu) {
    case 4: /* 0x0C70 modulation */ _cmd_modulation(drv, slot_no); break;
    case 5: /* 0x0B04 pan */ _cmd_pan(drv, slot_no); break;
    case 6: /* 0x0AC2 all notes off */ _cmd_all_notes_off(drv, slot_no); break;
    case 7: /* 0x0AD7 sysex A */ _cmd_sysex_system(drv, slot_no, 0); break;
    case 8: /* 0x0AE3 sysex B */ _cmd_sysex_system(drv, slot_no, 1); break;
    case 0x0B: /* 0x0B44 jump: order = param-1, advance <- NOT advance */
        rec->order = (uint8_t)(s->param - 1u);
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0C: /* 0x0B54 volume = min(param, 0x40), then CC 7 */
        if (s->param >= 0x40u) s->param = 0x40u;
        s->volume = s->param;
        if (drv->track_volume[_channel(slot_no)] != s->volume) {
            drv->track_volume[_channel(slot_no)] = s->volume;
            _midi(drv, (uint8_t)(0xB0u + _channel(slot_no)), 0x07u,
                  _cc_volume(s->volume));
        }
        break;
    case 0x0D: /* 0x0B3C advance <- NOT advance */
        rec->advance = (uint8_t)~rec->advance;
        break;
    case 0x0E: /* 0x0891 mark the record finished */
        rec->finished = 0xFFu;
        break;
    case 0x0F: /* 0x0B67 ticks per row, param & 0x1F, 0 ignored */
        if ((s->param & 0x1Fu) != 0) {
            rec->tick = 0;
            rec->ticks_per_row = (uint8_t)(s->param & 0x1Fu);
        }
        break;
    default: break;
    }
}

/**
 * @brief Play one decoded slot cell (0x0949).
 *
 * @param drv the driver
 * @param rec the record being stepped
 * @param slot_no the slot
 */
static void _play_cell(SoundDriver *drv, Mt32Record *rec, unsigned slot_no)
{
    Mt32Slot *s = &drv->slot[slot_no];
    Mt32Play p;
    unsigned ch = _channel(slot_no);

    p.drv = drv;
    p.rec = rec;

    if (s->instrument != 0) {
        int any_note = (s->note[0] | s->note[1] | s->note[2] | s->note[3]) != 0;
        drv->command_block[slot_no] = s->instrument;
        if (any_note) {
            unsigned index = (unsigned)s->instrument - 1u;
            uint8_t volume;
            if (index >= 64u) index = 63u;
            volume = drv->instruments[2u * index + 1u]; /* header high byte */
            volume = (uint8_t)(volume << 1);
            if (volume >= 0x80u) volume = 0x7Fu;
            if (s->command == 0x0Cu) {
                volume = (uint8_t)(s->param << 1);
                if (volume >= 0x80u) volume = 0x7Fu;
                s->command = 0;
                s->param = 0;
            }
            if (drv->track_volume[ch] != volume) {
                drv->track_volume[ch] = volume;
                _midi(drv, (uint8_t)(0xB0u + ch), 0x07u, volume);
            }
        }
        if (s->instrument - 1u != s->last_program) {
            s->last_program = (uint8_t)(s->instrument - 1u);
            _program(drv, (uint8_t)ch, s->last_program);
        }
    }

    if ((s->note[0] | s->note[1] | s->note[2] | s->note[3]) == 0) {
        _note_command(&p, slot_no);
        return;
    }
    if ((s->command & 0x1Fu) == 3u) {
        _slide_setup(drv, slot_no);
        return;
    }
    _note_command(&p, slot_no);
    _note_on(drv, slot_no, s->note[0]);
    _note_on_extra(drv, slot_no, s->note[1]);
    _note_on_extra(drv, slot_no, s->note[2]);
    _note_on_extra(drv, slot_no, s->note[3]);
}

/**
 * @brief The row's class payload sizes, CS:0x13F8 = {0,2,2,7}.
 *
 * @param cls the 2-bit class
 * @return the payload byte count
 */
static unsigned _class_size(unsigned cls)
{
    static const uint8_t size[4] = { 0, 2, 2, 7 };
    return size[cls & 3u];
}

/**
 * @brief Store one class's payload in a slot cell (0x0896's track loop).
 *
 * @param drv the driver
 * @param slot_no the slot
 * @param cursor the pattern-relative cursor, advanced by the payload
 * @param cls the class
 */
static void _store_cell(SoundDriver *drv, unsigned slot_no, uint32_t *cursor,
                        unsigned cls)
{
    Mt32Slot *s = &drv->slot[slot_no];
    uint32_t c = *cursor;

    switch (cls & 3u) {
    case 0:
        memset(s->note, 0, 4);
        s->instrument = 0;
        s->command = 0;
        s->param = 0;
        break;
    case 1:
        s->note[0] = s->note[1] = s->note[2] = s->note[3] = 0;
        s->instrument = 0;
        s->command = _u8(drv, c);
        s->param = _u8(drv, c + 1u);
        *cursor = c + 2u;
        break;
    case 2:
        s->note[0] = _u8(drv, c);
        s->note[1] = s->note[2] = s->note[3] = 0;
        s->instrument = _u8(drv, c + 1u);
        s->command = 0;
        s->param = 0;
        *cursor = c + 2u;
        break;
    default:
        s->note[0] = _u8(drv, c);
        s->note[1] = _u8(drv, c + 1u);
        s->note[2] = _u8(drv, c + 2u);
        s->note[3] = _u8(drv, c + 3u);
        s->instrument = _u8(drv, c + 4u);
        s->command = _u8(drv, c + 5u);
        s->param = _u8(drv, c + 6u);
        *cursor = c + 7u;
        break;
    }
}

/**
 * @brief Decode one row of the current pattern into the slot cells (0x0896).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _decode_row(SoundDriver *drv, Mt32Record *rec)
{
    uint32_t pattern;
    uint32_t base;
    uint32_t cursor;
    uint32_t post_mask;
    uint16_t mask;
    uint16_t original;
    unsigned i;

    rec->row++;
    if (rec->order >= 0x7Fu) {
        rec->row = MT32_ROWS;
        return;
    }
    pattern = drv->order_list[rec->order];
    if (pattern >= drv->offset_count) {
        rec->row = MT32_ROWS;
        return;
    }
    base = drv->off_base + (uint32_t)drv->offset[pattern] * 16u;
    cursor = base + rec->cursor;
    mask = _u16(drv, cursor);
    original = mask;
    cursor += 2u;
    post_mask = cursor;

    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++) {
        unsigned cls = (unsigned)(mask >> 14) & 3u;
        mask = (uint16_t)(mask << 2);
        _store_cell(drv, rec->track_slot[i] & 7u, &cursor, cls);
    }
    /* 0x0920: the cursor is restored to the post-mask byte and the driver
     * re-sums the first header[0x118] classes of the same mask — the file
     * encodes exactly that many classes, and their sizes are the payload. */
    cursor = post_mask;
    mask = original;
    for (i = 0; i < drv->trailing; i++) {
        cursor += _class_size((unsigned)(mask >> 14) & 3u);
        mask = (uint16_t)(mask << 2);
    }
    rec->cursor = (uint16_t)(cursor - base);
}

/**
 * @brief Play the cells the row decoder just filled (0x0810's second loop).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _play_row(SoundDriver *drv, Mt32Record *rec)
{
    unsigned i;

    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++)
        _play_cell(drv, rec, rec->track_slot[i] & 7u);
}

/**
 * @brief Release a record and its slots (0x03D9).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _release_record(SoundDriver *drv, Mt32Record *rec)
{
    unsigned i;

    if (!rec->active) return;
    rec->active = 0;
    for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++) {
        unsigned slot = rec->track_slot[i] & 7u;
        drv->slot_free[slot] = (uint8_t)slot;
        _all_notes_off(drv, _channel(slot));
    }
}

/**
 * @brief The end-of-pattern block (0x0856) and the advance gate (0x0889).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _end_of_pattern(SoundDriver *drv, Mt32Record *rec)
{
    rec->tick = 0;
    rec->advance = 0;
    rec->row = 0;
    rec->cursor = 0;
    rec->order++;
    if (rec->order != rec->last_order) return;
    rec->finished = 0xFFu;
    if (rec->loop) {
        rec->order = rec->first_order;
        return;
    }
    _release_record(drv, rec);
}

/**
 * @brief Step one record by one driver tick (0x07C9).
 *
 * @param drv the driver
 * @param rec the record
 */
static void _step_record(SoundDriver *drv, Mt32Record *rec)
{
    unsigned i;

    if (!rec->active) return;
    rec->tick++;
    if (rec->tick < rec->ticks_per_row) {
        Mt32Play p;
        p.drv = drv;
        p.rec = rec;
        for (i = 0; i < rec->tracks && i < MT32_SLOTS; i++)
            _tick_command(&p, rec->track_slot[i] & 7u);
        return;
    }
    rec->tick = 0;
    _decode_row(drv, rec);
    _play_row(drv, rec);
    if (rec->row == MT32_ROWS) _end_of_pattern(drv, rec);
    if (rec->active && rec->advance) _end_of_pattern(drv, rec);
}

/**
 * @brief Send one timbre block as six DT1 messages (0x06A7).
 *
 * The caller advances the bank pointer past every flagged instrument, so the
 * block offset is passed in.  A block whose first common byte is a space is a
 * placeholder and is not sent (0x069B returns straight away).
 *
 * @param drv the driver
 * @param index the instrument index (0-based), the timbre-memory number
 * @param block the byte offset of the 256-byte block in the tune
 */
static void _send_timbre(SoundDriver *drv, unsigned index, uint32_t block)
{
    uint8_t common[8];
    uint8_t data[0x3A];
    uint8_t hi = (uint8_t)((index * 8u) >> 7);
    uint8_t lo = (uint8_t)((index * 8u) & 0x7Fu);
    uint8_t mid = (uint8_t)(index * 2u);
    uint8_t low_addr = 0x0Au;
    unsigned i;

    for (i = 0; i < 8u; i++) common[i] = _u8(drv, block + i);
    if (common[0] == 0x20u) return;

    memcpy(data, common, 8);
    if (common[0] != 0x03u) data[1] = (uint8_t)index;
    _sysex(drv, 0x05u, hi, lo, data, 8u);

    for (i = 0; i < 4u; i++) data[i] = _u8(drv, block + 0x12u + i);
    _sysex(drv, 0x08u, mid, low_addr, data, 4u);
    low_addr = (uint8_t)(low_addr + 4u);

    for (i = 0; i < 4u; i++) {
        unsigned j;
        for (j = 0; j < 0x3Au; j++)
            data[j] = _u8(drv, block + 0x16u + i * 0x3Au + j);
        _sysex(drv, 0x08u, mid, low_addr, data, 0x3Au);
        low_addr = (uint8_t)(low_addr + 0x3Au);
        if (low_addr > 0x7Fu) {
            low_addr = (uint8_t)(low_addr & 0x7Fu);
            mid++;
        }
    }
}

/**
 * @brief The timbre dump + part-1 program cycle after a load (0x065A).
 *
 * @param drv the driver
 */
static void _init_timbres(SoundDriver *drv)
{
    unsigned block = 0;
    unsigned i;

    for (i = 0; i < 64u; i++) {
        if (drv->instruments[2u * i] == 0) continue;
        _send_timbre(drv, i, drv->bank_base + block * 0x100u);
        block++;
    }
    /* the driver's final reset: programs 0..63 on channel 2 (part 1) */
    for (i = 0; i < 64u; i++) _program(drv, 1u, (uint8_t)i);
}

/**
 * @brief The MT-32 reset sequence (0x05A4): volume, system block, CCs.
 *
 * @param drv the driver
 */
static void _reset_voices(SoundDriver *drv)
{
    /* CS:0x0EAD's data: address 10 00 04, then the 8 bytes the routine copies
     * from CS:0x0F0E (entry 7), then the image's trailing zero. */
    static const uint8_t system[9] = { 0x04, 0x04, 0x04, 0x04, 0x04,
                                       0x04, 0x04, 0x04, 0x00 };
    unsigned i;

    _send_master_volume(drv);
    memset(drv->record, 0, sizeof(drv->record));
    for (i = 0; i < MT32_SLOTS; i++) drv->slot_free[i] = (uint8_t)i;
    _sysex(drv, 0x10u, 0x00u, 0x04u, system, sizeof(system));
    for (i = 1; i <= 8u; i++) _all_notes_off(drv, i);
}

/**
 * @brief Read one whole file into memory.
 *
 * @param path the file
 * @param size receives the byte count
 * @return the buffer, or NULL
 */
static uint8_t *_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/**
 * @brief Read every regular file of a directory into the driver's ROM list.
 *
 * @param drv the driver
 * @param dir the directory
 */
static void _load_rom_dir(SoundDriver *drv, const char *dir)
{
    char path[1200];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t size;
        uint8_t *buf;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        buf = _read_file(path, &size);
        if (!buf) continue;
        if (drv->rom_count < MT32_ROMS_MAX) {
            drv->rom[drv->rom_count] = buf;
            drv->rom_size[drv->rom_count] = size;
            drv->rom_count++;
        } else {
            free(buf);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        size_t size;
        uint8_t *buf;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        buf = _read_file(path, &size);
        if (!buf) continue;
        if (drv->rom_count < MT32_ROMS_MAX) {
            drv->rom[drv->rom_count] = buf;
            drv->rom_size[drv->rom_count] = size;
            drv->rom_count++;
        } else {
            free(buf);
        }
    }
    closedir(d);
#endif
}

/**
 * @brief Hand the preferred ROM pair to mt32emu, pairing split images as a
 * fallback.
 *
 * @param drv the driver
 * @return 1 when a control and a PCM ROM were accepted
 */
static int _add_roms(SoundDriver *drv)
{
    const unsigned pairs =
        (unsigned)(sizeof(_rom_preference) / sizeof(_rom_preference[0]));
    mt32emu_rom_info info;
    unsigned i;
    unsigned j;

    for (i = 0; i < drv->rom_count; i++) {
        memset(&info, 0, sizeof(info));
        (void)mt32emu_identify_rom_data(&info, drv->rom[i], drv->rom_size[i],
                                        NULL);
        drv->rom_control[i] = info.control_rom_id;
        drv->rom_pcm[i] = info.pcm_rom_id;
    }

    /* The newest MT-32 model whose full control and PCM images are both in
     * the directory.  This makes a multi-ROM set deterministic instead of
     * whichever file readdir happened to return last. */
    for (i = 0; i < pairs; i++) {
        int c = -1;
        int p = -1;
        for (j = 0; j < drv->rom_count; j++) {
            if (c < 0 && drv->rom_control[j] &&
                strcmp(drv->rom_control[j], _rom_preference[i][0]) == 0)
                c = (int)j;
            if (p < 0 && drv->rom_pcm[j] &&
                strcmp(drv->rom_pcm[j], _rom_preference[i][1]) == 0)
                p = (int)j;
        }
        if (c < 0 || p < 0) continue;
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[c], drv->rom_size[c],
                                   NULL);
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[p], drv->rom_size[p],
                                   NULL);
        return 1;
    }

    /* No full pair: hand everything over and let the pairing entry point
     * merge MAME-style split images. */
    for (i = 0; i < drv->rom_count; i++)
        (void)mt32emu_add_rom_data(drv->ctx, drv->rom[i], drv->rom_size[i],
                                   NULL);
    mt32emu_get_rom_info(drv->ctx, &info);
    if (info.control_rom_id && info.pcm_rom_id) return 1;

    for (i = 0; i < drv->rom_count; i++) {
        for (j = 0; j < drv->rom_count; j++) {
            if (i == j) continue;
            (void)mt32emu_merge_and_add_rom_data(
                drv->ctx, drv->rom[i], drv->rom_size[i], NULL, drv->rom[j],
                drv->rom_size[j], NULL);
            mt32emu_get_rom_info(drv->ctx, &info);
            if (info.control_rom_id && info.pcm_rom_id) return 1;
        }
    }
    return 0;
}

/**
 * @brief The emulator's report callbacks: silence debug and LCD chatter.
 *
 * A NULL handler makes some control ROMs write their LCD text to stderr, so
 * the port installs a handler that drops everything except the two ROM
 * errors (which create() already reports through the return code).
 */
static mt32emu_report_handler_version MT32EMU_C_CALL
_report_version(mt32emu_report_handler_i i)
{
    (void)i;
    return MT32EMU_REPORT_HANDLER_VERSION_0;
}

static void MT32EMU_C_CALL _report_debug(void *data, const char *fmt,
                                         va_list list)
{
    (void)data;
    (void)fmt;
    (void)list;
}

static void MT32EMU_C_CALL _report_error_control(void *data) { (void)data; }
static void MT32EMU_C_CALL _report_error_pcm(void *data) { (void)data; }

static void MT32EMU_C_CALL _report_lcd(void *data, const char *message)
{
    (void)data;
    (void)message;
}

static void MT32EMU_C_CALL _report_midi_played(void *data) { (void)data; }

static mt32emu_boolean MT32EMU_C_CALL _report_queue_overflow(void *data)
{
    (void)data;
    return MT32EMU_BOOL_FALSE;
}

static void MT32EMU_C_CALL _report_realtime(void *data, mt32emu_bit8u byte)
{
    (void)data;
    (void)byte;
}

static void MT32EMU_C_CALL _report_reset(void *data) { (void)data; }
static void MT32EMU_C_CALL _report_reconfig(void *data) { (void)data; }

static void MT32EMU_C_CALL _report_reverb_mode(void *data, mt32emu_bit8u mode)
{
    (void)data;
    (void)mode;
}

static void MT32EMU_C_CALL _report_reverb_time(void *data, mt32emu_bit8u time)
{
    (void)data;
    (void)time;
}

static void MT32EMU_C_CALL _report_reverb_level(void *data, mt32emu_bit8u level)
{
    (void)data;
    (void)level;
}

static void MT32EMU_C_CALL _report_poly(void *data, mt32emu_bit8u part)
{
    (void)data;
    (void)part;
}

static void MT32EMU_C_CALL _report_program(void *data, mt32emu_bit8u part,
                                           const char *group, const char *patch)
{
    (void)data;
    (void)part;
    (void)group;
    (void)patch;
}

static const mt32emu_report_handler_i_v0 _report_handler = {
    _report_version,        _report_debug,       _report_error_control,
    _report_error_pcm,      _report_lcd,         _report_midi_played,
    _report_queue_overflow, _report_realtime,    _report_reset,
    _report_reconfig,       _report_reverb_mode, _report_reverb_time,
    _report_reverb_level,   _report_poly,        _report_program
};

/**
 * @brief The install reset (0x0CC7), minus the MPU-401 mode byte.
 *
 * `mt32_mpu_reset` sends 0x3F to the MPU (meaningless to the emulator), then
 * the MT-32 all-parameters reset, a CC/ bend reset on channels 1..9 and the
 * system block that clears the reverb and sets the partial reserve.  Without
 * it the emulator keeps its power-on reverb (5/3) instead of the driver's
 * post-install state (off).
 *
 * @param drv the driver
 */
static void _install_reset(SoundDriver *drv)
{
    static const uint8_t system[12] = { 0x00, 0x00, 0x00, 0x08, 0x08, 0x08,
                                        0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t zero = 0x00u;
    unsigned i;

    _sysex(drv, 0x7Fu, 0x00u, 0x00u, &zero, 1u);
    for (i = 1; i <= 9u; i++) {
        _midi(drv, (uint8_t)(0xB0u + i), 0x7Bu, 0x00u);
        _midi(drv, (uint8_t)(0xB0u + i), 0x40u, 0x00u);
        _midi(drv, (uint8_t)(0xE0u + i), 0x00u, 0x40u);
    }
    _sysex(drv, 0x10u, 0x00u, 0x01u, system, sizeof(system));
}

/**
 * @brief Install the MT-32 emulation (INT 60h install, 0x00DE).
 *
 * @param sampleRate the output rate in Hz the caller will render at
 * @return the driver, or NULL when the ROMs cannot be loaded
 */
static SoundDriver *_mt32_create(uint32_t sampleRate)
{
    mt32emu_report_handler_i report;
    SoundDriver *drv;
    unsigned i;

    if (_rom_dir[0] == '\0') {
        fprintf(stderr, "audio(mt32): no ROM directory given\n");
        return NULL;
    }
    drv = calloc(1, sizeof(*drv));
    if (!drv) return NULL;
    if (sampleRate == 0) sampleRate = 48000;
    drv->sampleRate = sampleRate;

    report.v0 = &_report_handler;
    drv->ctx = mt32emu_create_context(report, NULL);
    if (!drv->ctx) {
        fprintf(stderr, "audio(mt32): cannot create the emulator context\n");
        free(drv);
        return NULL;
    }
    mt32emu_set_stereo_output_samplerate(drv->ctx, (double)sampleRate);
    /* The same emulator settings DOSBox Staging uses, so the two play the
     * same instrument: accurate analog stage, 16-bit renderer, NICE DAC,
     * the three "nice" quality switches, and immediate MIDI. */
    mt32emu_set_analog_output_mode(drv->ctx, MT32EMU_AOM_ACCURATE);
    mt32emu_select_renderer_type(drv->ctx, MT32EMU_RT_BIT16S);
    mt32emu_set_dac_input_mode(drv->ctx, MT32EMU_DAC_NICE);
    mt32emu_set_nice_panning_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_nice_amp_ramp_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_nice_partial_mixing_enabled(drv->ctx, MT32EMU_BOOL_TRUE);
    mt32emu_set_midi_delay_mode(drv->ctx, MT32EMU_MDM_IMMEDIATE);
    /* The dry and reverb channels have separate gain factors; scaling both
     * is what makes the whole output 20 % quieter. */
    mt32emu_set_output_gain(drv->ctx, MT32_OUTPUT_GAIN);
    mt32emu_set_reverb_output_gain(drv->ctx, MT32_OUTPUT_GAIN);
    _load_rom_dir(drv, _rom_dir);
    if (drv->rom_count == 0 || !_add_roms(drv)) {
        fprintf(stderr, "audio(mt32): no control+PCM ROM pair found in '%s'\n",
                _rom_dir);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) free(drv->rom[i]);
        free(drv);
        return NULL;
    }
    if (mt32emu_open_synth(drv->ctx) != MT32EMU_RC_OK) {
        fprintf(stderr,
                "audio(mt32): cannot open the synth with the ROMs in '%s'\n",
                _rom_dir);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) free(drv->rom[i]);
        free(drv);
        return NULL;
    }
    if (mt32emu_get_actual_stereo_output_samplerate(drv->ctx) != sampleRate) {
        fprintf(stderr, "audio(mt32): mt32emu did not take %u Hz\n",
                sampleRate);
        mt32emu_close_synth(drv->ctx);
        mt32emu_free_context(drv->ctx);
        for (i = 0; i < drv->rom_count; i++) free(drv->rom[i]);
        free(drv);
        return NULL;
    }

    drv->volume = 0x4Bu;
    drv->system_block[0] = 0x00u; /* CS:0x0EAA..0x0EAC's image bytes */
    drv->system_block[1] = 0x04u;
    drv->system_block[2] = 0x04u;
    for (i = 0; i < MT32_SLOTS; i++) {
        drv->slot_free[i] = (uint8_t)i;
        drv->slot[i].last_program = 0xFFu;
    }
    _install_reset(drv); /* 0x00DE's mt32_probe -> mt32_mpu_reset */
    drv->ready = 1;
    {
        mt32emu_rom_info info;
        mt32emu_get_rom_info(drv->ctx, &info);
        fprintf(
            stderr, "audio(mt32): mt32emu %s, %s + %s, gain %.2f, %u Hz\n",
            mt32emu_get_library_version_string(), info.control_rom_id,
            info.pcm_rom_id, (double)MT32_OUTPUT_GAIN,
            (unsigned)mt32emu_get_actual_stereo_output_samplerate(drv->ctx));
    }
    return drv;
}

/**
 * @brief cmd 0: uninstall (0x0178).
 *
 * @param drv the driver; NULL is accepted
 */
static void _mt32_destroy(SoundDriver *drv)
{
    unsigned i;

    if (!drv) return;
    mt32emu_close_synth(drv->ctx);
    mt32emu_free_context(drv->ctx);
    for (i = 0; i < drv->rom_count; i++) free(drv->rom[i]);
    free(drv->tune);
    free(drv);
}

/**
 * @brief cmd 0x0A: the master volume (0x01CD).
 *
 * @param drv the driver
 * @param tempo CL, the game's byte; clamped to 100
 */
static void _mt32_set_tempo(SoundDriver *drv, uint8_t tempo)
{
    if (!drv || !drv->ready) return;
    if (tempo == drv->volume) return;
    if (tempo > 100u) tempo = 100u;
    drv->volume = tempo;
    _send_master_volume(drv);
}

/**
 * @brief cmd 0x0E: identify, the 8-byte command block (0x01BB).
 *
 * @param drv the driver
 * @return the block (CS:0x15D1)
 */
static const uint8_t *_mt32_command_block(SoundDriver *drv)
{
    static const uint8_t empty[8];
    if (!drv) return empty;
    return drv->command_block;
}

/**
 * @brief cmd 0x0C: load a tune image (0x0236).
 *
 * @param drv the driver
 * @param image the .RLD image
 * @param size its byte length
 */
static void _mt32_load_tune(SoundDriver *drv, const uint8_t *image,
                            uint32_t size)
{
    unsigned max_order = 0;
    unsigned patterns;
    unsigned i;

    if (!drv || !drv->ready || !image || size < MT32_HEADER) return;

    drv->off_base = rd_le_u16(image) >= 0x9AB6u ? MT32_OFF_NEW : MT32_OFF_OLD;
    drv->record_base = drv->off_base - 0x800u;
    if (drv->off_base + 0x200u > size || drv->record_base + 0x800u > size)
        return;

    free(drv->tune);
    drv->tune = malloc(size);
    if (!drv->tune) {
        drv->tune_size = 0;
        return;
    }
    memcpy(drv->tune, image, size);
    drv->tune_size = size;

    memcpy(drv->order_list, image + 0x18u, sizeof(drv->order_list));
    memcpy(drv->instruments, image + 0x98u, sizeof(drv->instruments));
    drv->trailing = image[(drv->off_base == MT32_OFF_NEW ? 0x118u : 0xD8u)];
    for (i = 0; i < 256u; i++)
        drv->offset[i] = rd_le_u16(image + drv->off_base + 2u * i);
    drv->offset_count = 256u;

    for (i = 0; i < 0x7Fu; i++)
        if (drv->order_list[i] > max_order) max_order = drv->order_list[i];
    patterns = max_order + 1u;
    if (patterns >= drv->offset_count) patterns = drv->offset_count - 1u;
    drv->bank_base = drv->off_base + (uint32_t)drv->offset[patterns] * 16u;

    _init_timbres(drv);  /* 0x0299 */
    _reset_voices(drv);  /* 0x029C */
}

/**
 * @brief Find and allocate a record's MIDI slots (0x0363).
 *
 * The original scans the eight free-list entries from the start and takes the
 * first `tracks` entries that are not `0xFF` (0x0396's `JZ` goes to `INC SI`,
 * not to the failure path); only running out of entries before the track count
 * is reached fails, which restores the whole list from the scratch copy.
 *
 * @param drv the driver
 * @param tracks the track count
 * @param slots receives the allocated slot numbers
 * @return 1 on success, 0 when the slots are full
 */
static int _alloc_record(SoundDriver *drv, unsigned tracks, uint8_t *slots)
{
    uint8_t scratch[MT32_SLOTS];
    unsigned taken = 0;
    unsigned i;

    if (tracks > MT32_SLOTS) tracks = MT32_SLOTS;
    memcpy(scratch, drv->slot_free, sizeof(scratch));
    for (i = 0; i < MT32_SLOTS && taken < tracks; i++) {
        if (drv->slot_free[i] == 0xFFu) continue;
        slots[taken] = drv->slot_free[i];
        drv->slot_free[i] = 0xFFu;
        _all_notes_off(drv, _channel(slots[taken]));
        taken++;
    }
    if (taken < tracks) {
        memcpy(drv->slot_free, scratch, sizeof(scratch));
        return 0;
    }
    return 1;
}

/**
 * @brief cmd 5: start a record (0x02A7).
 *
 * @param drv the driver
 * @param event CX, the record id in the loaded image
 * @return the record handle, or 0xFFFF when refused
 */
static uint16_t _mt32_start_event(SoundDriver *drv, uint16_t event)
{
    Mt32Record *rec = NULL;
    uint32_t off;
    unsigned tracks;
    unsigned i;

    if (!drv || !drv->ready || !drv->tune) return 0xFFFFu;
    if (event >= 128u) return 0xFFFFu;
    off = drv->record_base + (uint32_t)event * 16u;
    if (off + 16u > drv->tune_size) return 0xFFFFu;
    for (i = 0; i < MT32_RECORDS; i++) {
        if (!drv->record[i].active) {
            rec = &drv->record[i];
            break;
        }
    }
    if (!rec) return 0xFFFFu;

    memset(drv->track_volume, 0, sizeof(drv->track_volume));
    tracks = _u8(drv, off + 0x0Du);
    memset(rec, 0, sizeof(*rec));
    if (!_alloc_record(drv, tracks, rec->track_slot)) return 0xFFFFu;

    rec->tracks = (uint8_t)(tracks > MT32_SLOTS ? MT32_SLOTS : tracks);
    rec->ticks_per_row = 6;
    rec->order = _u8(drv, off + 0x0Au);
    rec->first_order = rec->order;
    rec->loop = _u8(drv, off + 0x0Cu);
    rec->last_order = (uint8_t)(_u8(drv, off + 0x0Bu) + 1u);
    rec->finished = 0;
    for (i = 0; i < MT32_SLOTS; i++) drv->slot[i].last_program = 0xFFu;
    rec->active = 0xFFu;
    return (uint16_t)(rec - drv->record);
}

/**
 * @brief cmd 6: release a record (0x03FA).
 *
 * @param drv the driver
 * @param voice the handle startEvent returned
 */
static void _mt32_release_event(SoundDriver *drv, uint16_t voice)
{
    if (!drv || voice >= MT32_RECORDS) return;
    _release_record(drv, &drv->record[voice]);
}

/**
 * @brief cmd 2: one sequencer tick (0x0792 -> 0x07A0).
 *
 * @param drv the driver
 */
static void _mt32_tick(SoundDriver *drv)
{
    unsigned i;

    if (!drv || !drv->ready) return;
    for (i = 0; i < MT32_RECORDS; i++) _step_record(drv, &drv->record[i]);
}

/**
 * @brief Render interleaved stereo samples at the requested rate.
 *
 * @param drv the driver
 * @param dst the destination, 2 * frames int16_t (L,R)
 * @param frames the frame count
 */
static void _mt32_generate(SoundDriver *drv, int16_t *dst, uint32_t frames)
{
    if (!drv || !drv->ctx) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }
    mt32emu_render_bit16s(drv->ctx, (mt32emu_bit16s *)dst, frames);
}

const SoundDriverModule g_sound_driver_mt32 = {
    _mt32_create,        _mt32_destroy,   _mt32_set_tempo,
    _mt32_command_block, _mt32_load_tune, _mt32_start_event,
    _mt32_release_event, _mt32_tick,      _mt32_generate
};
