/*
 * fanpro - SMC value codecs.
 *
 * The SMC reports a type fourcc per key at runtime; that fourcc, never the
 * platform, decides the encoding.  Integer and fixed-point types are
 * big-endian on the wire.  'flt ' is an IEEE-754 single in little-endian byte
 * order (this is what Apple Silicon fan keys use; Intel fan keys are 'fpe2').
 *
 * Fixed-point types are named fpXY / spXY where X and Y are hex digits giving
 * the integer and fraction bit counts: 'fpe2' is 14.2 unsigned, 'sp78' is
 * 7.8 signed plus a sign bit.  Parsing the digits out of the fourcc handles
 * every variant uniformly instead of enumerating a table.
 */
#include "fanpro/smc_codec.h"

#include <math.h>
#include <string.h>

uint32_t
fanpro_fourcc(const char s[4])
{
	return ((uint32_t)(unsigned char)s[0] << 24) |
	       ((uint32_t)(unsigned char)s[1] << 16) |
	       ((uint32_t)(unsigned char)s[2] << 8) |
	       ((uint32_t)(unsigned char)s[3]);
}

void
fanpro_fourcc_str(uint32_t v, char out[5])
{
	int i;

	for (i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)((v >> (8 * (3 - i))) & 0xff);
		out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	out[4] = '\0';
}

/* Hex digit value, or -1. */
static int
hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Recognise fpXY / spXY.  On success fills the fraction bit count and the
 * expected byte width, and reports whether the type is signed.
 *
 * Unsigned fpXY: X + Y == 16.  Signed spXY: X + Y == 15, the top bit is sign.
 */
static bool
parse_fixed(uint32_t type, int *frac_bits, bool *is_signed, uint32_t *width)
{
	unsigned char c0 = (unsigned char)((type >> 24) & 0xff);
	unsigned char c1 = (unsigned char)((type >> 16) & 0xff);
	int hi = hex_digit((unsigned char)((type >> 8) & 0xff));
	int lo = hex_digit((unsigned char)(type & 0xff));

	if (c1 != 'p' || hi < 0 || lo < 0)
		return false;

	if (c0 == 'f') {
		if (hi + lo != 16)
			return false;
		*is_signed = false;
	} else if (c0 == 's') {
		if (hi + lo != 15)
			return false;
		*is_signed = true;
	} else {
		return false;
	}

	*frac_bits = lo;
	*width = 2;
	return true;
}

/* Natural byte width of an integer type fourcc, or 0 if not an integer. */
static uint32_t
int_width(uint32_t type)
{
	if (type == FANPRO_TYPE_UI8 || type == FANPRO_TYPE_SI8)
		return 1;
	if (type == FANPRO_TYPE_UI16 || type == FANPRO_TYPE_SI16)
		return 2;
	if (type == FANPRO_TYPE_UI32)
		return 4;
	return 0;
}

/* Big-endian unsigned load of up to 4 bytes. */
static uint32_t
load_be(const uint8_t *b, uint32_t len)
{
	uint32_t v = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		v = (v << 8) | b[i];
	return v;
}

/* Big-endian unsigned store of up to 4 bytes. */
static void
store_be(uint8_t *b, uint32_t len, uint32_t v)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		b[len - 1 - i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

/* Sign-extend the low `bits` of v. */
static int32_t
sign_extend(uint32_t v, int bits)
{
	uint32_t mask = 1u << (bits - 1);

	if (v & mask)
		return (int32_t)(v | ~((1u << bits) - 1u));
	return (int32_t)v;
}

bool
fanpro_smc_decode(uint32_t type, const uint8_t *bytes, uint32_t len, double *out)
{
	int frac_bits;
	bool is_signed;
	uint32_t width;

	if (bytes == NULL || out == NULL || len == 0 || len > 4)
		return false;

	if (type == FANPRO_TYPE_FLT) {
		float f;

		if (len != 4)
			return false;
		/* Little-endian on the wire; memcpy avoids a strict-aliasing
		 * violation and any alignment assumption. */
		memcpy(&f, bytes, sizeof(f));
		*out = (double)f;
		return true;
	}

	if (type == FANPRO_TYPE_FLAG) {
		if (len != 1)
			return false;
		*out = bytes[0] ? 1.0 : 0.0;
		return true;
	}

	/* Integer widths are validated rather than inferred from `len`.  We
	 * take data_size from the firmware's key-info; if that ever disagrees
	 * with the type, decoding a ui32 as a ui16 would silently produce a
	 * plausible wrong number. */
	if (type == FANPRO_TYPE_UI8 || type == FANPRO_TYPE_UI16 ||
	    type == FANPRO_TYPE_UI32) {
		if (len != int_width(type))
			return false;
		*out = (double)load_be(bytes, len);
		return true;
	}

	if (type == FANPRO_TYPE_SI8 || type == FANPRO_TYPE_SI16) {
		if (len != int_width(type))
			return false;
		*out = (double)sign_extend(load_be(bytes, len), (int)(len * 8));
		return true;
	}

	if (parse_fixed(type, &frac_bits, &is_signed, &width)) {
		uint32_t raw;

		if (len != width)
			return false;
		raw = load_be(bytes, len);
		if (is_signed)
			*out = (double)sign_extend(raw, 16) / (double)(1u << frac_bits);
		else
			*out = (double)raw / (double)(1u << frac_bits);
		return true;
	}

	return false;
}

bool
fanpro_smc_encode(uint32_t type, double value, uint8_t *bytes, uint32_t len)
{
	int frac_bits;
	bool is_signed;
	uint32_t width;

	if (bytes == NULL || len == 0 || len > 4)
		return false;
	if (isnan(value) || isinf(value))
		return false;

	if (type == FANPRO_TYPE_FLT) {
		float f;

		if (len != 4)
			return false;
		f = (float)value;
		/* The isfinite check above ran on the double.  A finite double
		 * too large for a float narrows to infinity, so re-check after
		 * the narrowing rather than send Inf to the hardware. */
		if (!isfinite(f))
			return false;
		memcpy(bytes, &f, sizeof(f));
		return true;
	}

	if (type == FANPRO_TYPE_FLAG) {
		if (len != 1)
			return false;
		bytes[0] = (value != 0.0) ? 1 : 0;
		return true;
	}

	if (type == FANPRO_TYPE_UI8 || type == FANPRO_TYPE_UI16 ||
	    type == FANPRO_TYPE_UI32) {
		double max = (len == 4) ? 4294967295.0
		                        : (double)((1ull << (len * 8)) - 1ull);
		double r = round(value);

		if (len != int_width(type))
			return false;
		if (r < 0.0 || r > max)
			return false;
		store_be(bytes, len, (uint32_t)r);
		return true;
	}

	if (type == FANPRO_TYPE_SI8 || type == FANPRO_TYPE_SI16) {
		double lim = (double)(1ull << (len * 8 - 1));
		double r = round(value);

		if (len != int_width(type))
			return false;
		if (r < -lim || r > lim - 1.0)
			return false;
		store_be(bytes, len, (uint32_t)((int32_t)r) &
		                     (uint32_t)((1ull << (len * 8)) - 1ull));
		return true;
	}

	if (parse_fixed(type, &frac_bits, &is_signed, &width)) {
		double scaled;

		if (len != width)
			return false;
		scaled = round(value * (double)(1u << frac_bits));
		if (is_signed) {
			if (scaled < -32768.0 || scaled > 32767.0)
				return false;
			store_be(bytes, len, (uint32_t)((int32_t)scaled) & 0xffffu);
		} else {
			if (scaled < 0.0 || scaled > 65535.0)
				return false;
			store_be(bytes, len, (uint32_t)scaled);
		}
		return true;
	}

	return false;
}
