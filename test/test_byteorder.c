/* test_byteorder.c - the byteorder module's known-value checks.
 *
 * Runs on any host: the expected byte layouts are absolute, so the same
 * assertions prove the little-endian and big-endian paths on a LE host and a
 * BE host alike. */
#include "byteorder.h"
#include "test_support.h"

#include <stdint.h>

/**
 * @brief Check every endian accessor against absolute byte layouts.
 */
static void test_byteorder(void)
{
    uint8_t b[4];

    /* the detection must have picked one of the two orders */
    TEST_ASSERT_MESSAGE(BYTEORDER_HOST_LITTLE == 0 ||
                            BYTEORDER_HOST_LITTLE == 1,
                        "the host order is decided");

    /* the byte layout is independent of the host */
    wr_le_u16(b, 0x1234u);
    TEST_ASSERT_TRUE_MESSAGE(b[0] == 0x34u && b[1] == 0x12u,
                             "wr_le_u16 lays out low byte first");
    wr_be_u16(b, 0x1234u);
    TEST_ASSERT_TRUE_MESSAGE(b[0] == 0x12u && b[1] == 0x34u,
                             "wr_be_u16 lays out high byte first");
    wr_le_u32(b, 0x12345678u);
    TEST_ASSERT_TRUE_MESSAGE(b[0] == 0x78u && b[1] == 0x56u && b[2] == 0x34u &&
                                 b[3] == 0x12u,
                             "wr_le_u32 lays out low byte first");
    wr_be_u32(b, 0x12345678u);
    TEST_ASSERT_TRUE_MESSAGE(b[0] == 0x12u && b[1] == 0x34u && b[2] == 0x56u &&
                                 b[3] == 0x78u,
                             "wr_be_u32 lays out high byte first");

    /* reads of a fixed byte pattern */
    b[0] = 0x34u;
    b[1] = 0x12u;
    TEST_ASSERT_TRUE_MESSAGE(rd_le_u16(b) == 0x1234u, "rd_le_u16");
    TEST_ASSERT_TRUE_MESSAGE(rd_le_i16(b) == 0x1234, "rd_le_i16 positive");
    TEST_ASSERT_TRUE_MESSAGE(rd_be_u16(b) == 0x3412u, "rd_be_u16");
    b[0] = 0xFFu;
    b[1] = 0xFFu;
    TEST_ASSERT_TRUE_MESSAGE(rd_le_i16(b) == -1, "rd_le_i16 sign-extends");
    TEST_ASSERT_TRUE_MESSAGE(rd_be_i16(b) == -1, "rd_be_i16 sign-extends");
    b[0] = 0x78u;
    b[1] = 0x56u;
    b[2] = 0x34u;
    b[3] = 0x12u;
    TEST_ASSERT_TRUE_MESSAGE(rd_le_u32(b) == 0x12345678u, "rd_le_u32");
    TEST_ASSERT_TRUE_MESSAGE(rd_be_u32(b) == 0x78563412u, "rd_be_u32");
    b[0] = 0xFFu;
    b[1] = 0xFFu;
    b[2] = 0xFFu;
    b[3] = 0xFFu;
    TEST_ASSERT_TRUE_MESSAGE(rd_le_i32(b) == -1, "rd_le_i32 sign-extends");
    TEST_ASSERT_TRUE_MESSAGE(rd_be_i32(b) == -1, "rd_be_i32 sign-extends");

    /* 8-bit values have no order */
    b[0] = 0x80u;
    TEST_ASSERT_TRUE_MESSAGE(rd_u8(b) == 0x80u, "u8 read");
    TEST_ASSERT_TRUE_MESSAGE(rd_i8(b) == -128, "i8 sign-extends");
    wr_i8(b, -2);
    TEST_ASSERT_TRUE_MESSAGE(b[0] == 0xFEu, "wr_i8");

    /* every writer round-trips through its reader */
    {
        int16_t s = -12345;
        int32_t l = -123456789;

        wr_le_i16(b, s);
        TEST_ASSERT_TRUE_MESSAGE(rd_le_i16(b) == s, "le i16 round trip");
        wr_be_i16(b, s);
        TEST_ASSERT_TRUE_MESSAGE(rd_be_i16(b) == s, "be i16 round trip");
        wr_le_i32(b, l);
        TEST_ASSERT_TRUE_MESSAGE(rd_le_i32(b) == l, "le i32 round trip");
        wr_be_i32(b, l);
        TEST_ASSERT_TRUE_MESSAGE(rd_be_i32(b) == l, "be i32 round trip");
    }
}

int main(int argc, char **argv)
{
    test_args_set(argc, argv);
    UNITY_BEGIN();
    RUN_TEST(test_byteorder);
    return UNITY_END();
}
