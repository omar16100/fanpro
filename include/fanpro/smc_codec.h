/*
 * fanpro - SMC value codecs.
 *
 * SMC values are typed by a fourcc reported at runtime by the key-info
 * command.  Encoding differs by platform: fan RPM keys are IEEE-754 float
 * (little-endian) on Apple Silicon and fpe2 14.2 fixed-point (big-endian) on
 * Intel.  Nothing here may assume a platform; the type fourcc decides.
 */
#ifndef FANPRO_SMC_CODEC_H
#define FANPRO_SMC_CODEC_H

#include <stdbool.h>
#include <stdint.h>

/* Type fourccs seen on fan and temperature keys. */
#define FANPRO_TYPE_FLT   fanpro_fourcc("flt ")
#define FANPRO_TYPE_FP1F  fanpro_fourcc("fp1f")
#define FANPRO_TYPE_FP4C  fanpro_fourcc("fp4c")
#define FANPRO_TYPE_FP5B  fanpro_fourcc("fp5b")
#define FANPRO_TYPE_FP6A  fanpro_fourcc("fp6a")
#define FANPRO_TYPE_FP79  fanpro_fourcc("fp79")
#define FANPRO_TYPE_FP88  fanpro_fourcc("fp88")
#define FANPRO_TYPE_FPA6  fanpro_fourcc("fpa6")
#define FANPRO_TYPE_FPC4  fanpro_fourcc("fpc4")
#define FANPRO_TYPE_FPE2  fanpro_fourcc("fpe2")
#define FANPRO_TYPE_SP78  fanpro_fourcc("sp78")
#define FANPRO_TYPE_UI8   fanpro_fourcc("ui8 ")
#define FANPRO_TYPE_UI16  fanpro_fourcc("ui16")
#define FANPRO_TYPE_UI32  fanpro_fourcc("ui32")
#define FANPRO_TYPE_SI8   fanpro_fourcc("si8 ")
#define FANPRO_TYPE_SI16  fanpro_fourcc("si16")
#define FANPRO_TYPE_HEX   fanpro_fourcc("hex_")
#define FANPRO_TYPE_CH8   fanpro_fourcc("ch8*")
#define FANPRO_TYPE_FLAG  fanpro_fourcc("flag")

/* Pack a 4-character key or type name into the big-endian fourcc the SMC
 * expects.  fanpro_fourcc("F0Ac") == 0x46304163. */
uint32_t fanpro_fourcc(const char s[4]);

/* Unpack into a NUL-terminated 5-byte buffer.  Non-printable bytes become '.'. */
void fanpro_fourcc_str(uint32_t v, char out[5]);

/*
 * Decode `len` wire bytes of the given type into a double.
 * Returns true if the type is understood, false otherwise (out untouched).
 */
bool fanpro_smc_decode(uint32_t type, const uint8_t *bytes, uint32_t len,
                       double *out);

/*
 * Encode a double into `len` wire bytes of the given type.
 * Returns true if the type is understood and the value fits the encoding.
 */
bool fanpro_smc_encode(uint32_t type, double value, uint8_t *bytes,
                       uint32_t len);

#endif /* FANPRO_SMC_CODEC_H */
