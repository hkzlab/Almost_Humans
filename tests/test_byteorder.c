/* test_byteorder.c — the byteorder module's known-value _check.
 *
 * Runs on any host: the expected byte layouts are absolute, so the same
 * assertions prove the little-endian and big-endian paths on a LE host and a
 * BE host alike. */
#include "byteorder.h"

#include <stdio.h>

static int32_t _bad;

static void _check(int32_t ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        _bad = 1;
    }
}

int main(void)
{
    uint8_t b[4];

    /* the detection must have picked one of the two orders */
    _check(BYTEORDER_HOST_LITTLE == 0 || BYTEORDER_HOST_LITTLE == 1,
           "the host order is decided");

    /* the byte layout is independent of the host */
    wr_le_u16(b, 0x1234u);
    _check(b[0] == 0x34u && b[1] == 0x12u, "wr_le_u16 lays out low byte first");
    wr_be_u16(b, 0x1234u);
    _check(b[0] == 0x12u && b[1] == 0x34u,
           "wr_be_u16 lays out high byte first");
    wr_le_u32(b, 0x12345678u);
    _check(b[0] == 0x78u && b[1] == 0x56u && b[2] == 0x34u && b[3] == 0x12u,
           "wr_le_u32 lays out low byte first");
    wr_be_u32(b, 0x12345678u);
    _check(b[0] == 0x12u && b[1] == 0x34u && b[2] == 0x56u && b[3] == 0x78u,
           "wr_be_u32 lays out high byte first");

    /* reads of a fixed byte pattern */
    b[0] = 0x34u;
    b[1] = 0x12u;
    _check(rd_le_u16(b) == 0x1234u, "rd_le_u16");
    _check(rd_le_i16(b) == 0x1234, "rd_le_i16 positive");
    _check(rd_be_u16(b) == 0x3412u, "rd_be_u16");
    b[0] = 0xFFu;
    b[1] = 0xFFu;
    _check(rd_le_i16(b) == -1, "rd_le_i16 sign-extends");
    _check(rd_be_i16(b) == -1, "rd_be_i16 sign-extends");
    b[0] = 0x78u;
    b[1] = 0x56u;
    b[2] = 0x34u;
    b[3] = 0x12u;
    _check(rd_le_u32(b) == 0x12345678u, "rd_le_u32");
    _check(rd_be_u32(b) == 0x78563412u, "rd_be_u32");
    b[0] = 0xFFu;
    b[1] = 0xFFu;
    b[2] = 0xFFu;
    b[3] = 0xFFu;
    _check(rd_le_i32(b) == -1, "rd_le_i32 sign-extends");
    _check(rd_be_i32(b) == -1, "rd_be_i32 sign-extends");

    /* 8-bit values have no order */
    b[0] = 0x80u;
    _check(rd_u8(b) == 0x80u, "u8 read");
    _check(rd_i8(b) == -128, "i8 sign-extends");
    wr_i8(b, -2);
    _check(b[0] == 0xFEu, "wr_i8");

    /* every writer round-trips through its reader */
    {
        int16_t s = -12345;
        int32_t l = -123456789;
        wr_le_i16(b, s);
        _check(rd_le_i16(b) == s, "le i16 round trip");
        wr_be_i16(b, s);
        _check(rd_be_i16(b) == s, "be i16 round trip");
        wr_le_i32(b, l);
        _check(rd_le_i32(b) == l, "le i32 round trip");
        wr_be_i32(b, l);
        _check(rd_be_i32(b) == l, "be i32 round trip");
    }

    if (_bad) {
        fprintf(stderr, "byteorder: FAIL\n");
        return 1;
    }
    printf("byteorder: ok\n");
    return 0;
}
