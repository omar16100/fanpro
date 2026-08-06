/*
 * Codec tests: known vectors first, then round trips.
 *
 * Known vectors matter more than round trips here.  A codec that is
 * self-consistently wrong (say, big-endian floats) round-trips perfectly and
 * still commands the wrong fan speed, so every encoding is pinned to the
 * exact bytes the firmware expects.
 */
#include "tinytest.h"

#include "fanpro/smc_codec.h"

#include <string.h>

TT_TEST(fourcc_pack)
{
	char s[5];

	TT_EQ_UINT(fanpro_fourcc("F0Ac"), 0x46304163u);
	TT_EQ_UINT(fanpro_fourcc("FNum"), 0x464e756du);
	TT_EQ_UINT(fanpro_fourcc("Ftst"), 0x46747374u);
	TT_EQ_UINT(fanpro_fourcc("flt "), 0x666c7420u);

	fanpro_fourcc_str(0x46304163u, s);
	TT_EQ_STR(s, "F0Ac");
	fanpro_fourcc_str(0x666c7420u, s);
	TT_EQ_STR(s, "flt ");

	/* Non-printable bytes must not corrupt the output buffer. */
	fanpro_fourcc_str(0x00010203u, s);
	TT_EQ_STR(s, "....");
}

TT_TEST(decode_flt_little_endian)
{
	/* 1500.0f == 0x44BB8000; little-endian on the wire. */
	const uint8_t rpm[4] = { 0x00, 0x80, 0xbb, 0x44 };
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FLT, rpm, 4, &v));
	TT_NEAR(v, 1500.0, 0.001);

	/* Wrong length must be refused, not silently misread. */
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_FLT, rpm, 2, &v));
}

TT_TEST(encode_flt_little_endian)
{
	uint8_t b[4] = { 0 };
	const uint8_t want[4] = { 0x00, 0x80, 0xbb, 0x44 };

	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FLT, 1500.0, b, 4));
	TT_ASSERT(memcmp(b, want, 4) == 0);
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, 1500.0, b, 2));
}

TT_TEST(fpe2_is_big_endian_14_2)
{
	/* Intel fan encoding: 1500 rpm -> 6000 -> 0x1770, big-endian. */
	const uint8_t wire[2] = { 0x17, 0x70 };
	uint8_t b[2] = { 0 };
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FPE2, wire, 2, &v));
	TT_NEAR(v, 1500.0, 0.001);

	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FPE2, 1500.0, b, 2));
	TT_ASSERT(memcmp(b, wire, 2) == 0);

	/* 14.2 tops out at 16383.75. */
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FPE2, 20000.0, b, 2));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FPE2, -1.0, b, 2));
}

TT_TEST(sp78_is_signed_7_8)
{
	/* 45.5 C -> 45.5 * 256 = 11648 = 0x2D80. */
	const uint8_t pos[2] = { 0x2d, 0x80 };
	const uint8_t neg[2] = { 0xff, 0x00 }; /* -1.0 */
	uint8_t b[2] = { 0 };
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_SP78, pos, 2, &v));
	TT_NEAR(v, 45.5, 0.001);

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_SP78, neg, 2, &v));
	TT_NEAR(v, -1.0, 0.001);

	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_SP78, 45.5, b, 2));
	TT_ASSERT(memcmp(b, pos, 2) == 0);
}

TT_TEST(fp88_and_fp4c_parse_from_fourcc)
{
	/* The codec derives the binary point from the type name rather than a
	 * lookup table, so a type it has never seen still decodes correctly. */
	const uint8_t wire[2] = { 0x01, 0x80 }; /* 0x0180 */
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FP88, wire, 2, &v));
	TT_NEAR(v, 1.5, 0.001); /* 384 / 256 */

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FP4C, wire, 2, &v));
	TT_NEAR(v, 384.0 / 4096.0, 0.000001);
}

TT_TEST(integers_are_big_endian)
{
	const uint8_t u8[1] = { 0x03 };
	const uint8_t u16[2] = { 0x01, 0x00 };
	const uint8_t u32[4] = { 0x00, 0x00, 0x01, 0x00 };
	const uint8_t s16[2] = { 0xff, 0xff };
	uint8_t b[4] = { 0 };
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_UI8, u8, 1, &v));
	TT_NEAR(v, 3.0, 0.0);
	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_UI16, u16, 2, &v));
	TT_NEAR(v, 256.0, 0.0);
	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_UI32, u32, 4, &v));
	TT_NEAR(v, 256.0, 0.0);
	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_SI16, s16, 2, &v));
	TT_NEAR(v, -1.0, 0.0);

	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_UI16, 256.0, b, 2));
	TT_EQ_UINT(b[0], 0x01);
	TT_EQ_UINT(b[1], 0x00);

	/* Range enforcement: mode keys are ui8, so 256 must not wrap to 0. */
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_UI8, 256.0, b, 1));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_UI8, -1.0, b, 1));
	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_UI8, 255.0, b, 1));
}

TT_TEST(flag_type)
{
	const uint8_t on[1] = { 0x01 };
	uint8_t b[1] = { 0 };
	double v = 0.0;

	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FLAG, on, 1, &v));
	TT_NEAR(v, 1.0, 0.0);
	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FLAG, 1.0, b, 1));
	TT_EQ_UINT(b[0], 1);
}

TT_TEST(unknown_type_is_refused)
{
	const uint8_t junk[4] = { 1, 2, 3, 4 };
	uint8_t b[4] = { 0 };
	double v = 12345.0;

	/* 'ch8*' is a string type; there is no numeric reading of it, and
	 * pretending otherwise would put garbage into a fan target. */
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_CH8, junk, 4, &v));
	TT_NEAR(v, 12345.0, 0.0); /* out must be untouched */
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_CH8, 1.0, b, 4));

	TT_FALSE(fanpro_smc_decode(fanpro_fourcc("zzzz"), junk, 4, &v));
}

TT_TEST(nan_and_inf_never_reach_the_wire)
{
	uint8_t b[4] = { 0 };
	double nan_v = 0.0 / 0.0;
	double inf_v = 1.0 / 0.0;

	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, nan_v, b, 4));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, inf_v, b, 4));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FPE2, nan_v, b, 2));
}

TT_TEST(round_trip_rpm_range)
{
	int rpm;

	for (rpm = 0; rpm <= 6000; rpm += 137) {
		uint8_t b[4] = { 0 };
		double v = 0.0;

		TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FLT, (double)rpm, b, 4));
		TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FLT, b, 4, &v));
		TT_NEAR(v, (double)rpm, 0.001);

		TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FPE2, (double)rpm, b, 2));
		TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_FPE2, b, 2, &v));
		TT_NEAR(v, (double)rpm, 0.25);
	}
}

TT_TEST(null_and_zero_length_are_refused)
{
	const uint8_t b[4] = { 0 };
	uint8_t out[4] = { 0 };
	double v = 0.0;

	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_FLT, NULL, 4, &v));
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_FLT, b, 4, NULL));
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_FLT, b, 0, &v));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, 1.0, NULL, 4));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, 1.0, out, 0));
}
