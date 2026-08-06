/*
 * Transport tests against the fake backend.
 *
 * The point of these is the return-code contract: a firmware verdict must
 * never be confused with a transport failure, because the whole unlock
 * strategy is built on reading 0x82 and 0x84 as information rather than as
 * errors.
 */
#include "tinytest.h"

#include "fanpro/smc.h"
#include "fanpro/smc_codec.h"
#include "fanpro/smc_fake.h"

#include <stdlib.h>

static fanpro_fake_t *g_fake;
static fanpro_smc_t   g_smc;

static void
setup(fanpro_fake_personality_t p, int fans, bool lowercase)
{
	g_fake = fanpro_fake_create(p);
	fanpro_fake_add_fans(g_fake, fans, 700.0, 5000.0, lowercase);
	fanpro_smc_open(&g_smc, fanpro_fake_backend(g_fake));
}

static void
teardown(void)
{
	fanpro_smc_close(&g_smc);
	fanpro_fake_destroy(g_fake);
	g_fake = NULL;
}

TT_TEST(smc_key_info_reports_runtime_type)
{
	fanpro_smc_key_info_t info;

	setup(FANPRO_FAKE_DIRECT_OK, 2, false);

	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("F0Ac"), &info),
	          FANPRO_SMC_SUCCESS);
	TT_EQ_UINT(info.data_type, FANPRO_TYPE_FLT);
	TT_EQ_UINT(info.data_size, 4);

	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("FNum"), &info),
	          FANPRO_SMC_SUCCESS);
	TT_EQ_UINT(info.data_type, FANPRO_TYPE_UI8);
	TT_EQ_UINT(info.data_size, 1);

	teardown();
}

TT_TEST(smc_absent_key_is_information_not_failure)
{
	fanpro_smc_key_info_t info;

	/* An M5-style machine has no Ftst.  The distinction between "0x84, so
	 * this generation does not need the unlock" and "transport broke" is
	 * what the whole unlock strategy hangs on. */
	setup(FANPRO_FAKE_DIRECT_OK, 2, true);

	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("Ftst"), &info),
	          FANPRO_SMC_NOT_FOUND);
	TT_ASSERT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("Ftst"), &info) >= 0);

	teardown();
}

TT_TEST(smc_read_decodes_by_type)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_DIRECT_OK, 2, false);
	fanpro_fake_poke(g_fake, "F0Ac", 1234.0);

	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Ac"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_TRUE(v.numeric);
	TT_NEAR(v.num, 1234.0, 0.001);
	TT_EQ_UINT(v.raw_len, 4);

	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("FNum"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 2.0, 0.0);

	teardown();
}

TT_TEST(smc_write_round_trips_through_the_wire)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_DIRECT_OK, 1, false);

	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Tg"), 1800.0),
	          FANPRO_SMC_SUCCESS);
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Tg"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 1800.0, 0.001);

	teardown();
}

TT_TEST(smc_mode3_rejects_direct_write_with_0x82)
{
	setup(FANPRO_FAKE_NEEDS_FTST, 2, false);

	/* This is the M3/M4 trap: the write is refused, and a tool that treats
	 * any non-zero as generic failure never discovers why. */
	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0),
	          FANPRO_SMC_BAD_COMMAND);

	teardown();
}

TT_TEST(smc_direct_write_succeeds_on_permissive_firmware)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_DIRECT_OK, 2, false);

	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0),
	          FANPRO_SMC_SUCCESS);
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Md"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 1.0, 0.0);

	teardown();
}

TT_TEST(smc_ftst_yields_after_the_delay_not_before)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_NEEDS_FTST, 2, false);
	fanpro_fake_set_yield_delay_ms(g_fake, 3500);

	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("Ftst"), 1.0),
	          FANPRO_SMC_SUCCESS);

	/* Still mode 3 a second in: polling must not give up early. */
	fanpro_fake_advance_ms(g_fake, 1000);
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Md"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 3.0, 0.0);
	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0),
	          FANPRO_SMC_BAD_COMMAND);

	fanpro_fake_advance_ms(g_fake, 3000);
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Md"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 0.0, 0.0);
	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0),
	          FANPRO_SMC_SUCCESS);

	teardown();
}

TT_TEST(smc_clearing_ftst_kills_every_manual_fan)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_NEEDS_FTST, 2, false);
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("Ftst"), 1.0);
	fanpro_fake_advance_ms(g_fake, 4000);
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0);
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("F1Md"), 1.0);

	/* Clearing Ftst while fans are still manual is the bug the refcount in
	 * unlock.c exists to prevent; the fake makes it visible. */
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("Ftst"), 0.0);

	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Md"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 3.0, 0.0);
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F1Md"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 3.0, 0.0);

	teardown();
}

TT_TEST(smc_target_only_reaches_the_blades_in_manual_mode)
{
	fanpro_smc_value_t ac;

	setup(FANPRO_FAKE_NEEDS_FTST, 1, false);

	/* Writing a target while the firmware still owns the fan is precisely
	 * the silent no-op that makes naive tools look like they work. */
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Tg"), 3000.0);
	fanpro_smc_read(&g_smc, fanpro_fourcc("F0Ac"), &ac);
	TT_NEAR(ac.num, 700.0, 0.001);

	fanpro_smc_write_num(&g_smc, fanpro_fourcc("Ftst"), 1.0);
	fanpro_fake_advance_ms(g_fake, 4000);
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Md"), 1.0);
	fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Tg"), 3000.0);

	fanpro_smc_read(&g_smc, fanpro_fourcc("F0Ac"), &ac);
	TT_NEAR(ac.num, 3000.0, 0.001);

	teardown();
}

TT_TEST(smc_0x87_on_target_still_applies_the_value)
{
	fanpro_smc_value_t v;

	setup(FANPRO_FAKE_DIRECT_OK, 1, false);
	fanpro_fake_force_result(g_fake, "F0Tg", FANPRO_SMC_KEY_SIZE_MISMATCH);

	/* Documented firmware quirk: the call errors and the value lands
	 * anyway.  Treating it as a hard failure causes a retry storm. */
	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Tg"), 2200.0),
	          FANPRO_SMC_KEY_SIZE_MISMATCH);

	fanpro_fake_clear_result(g_fake, "F0Tg");
	TT_EQ_INT(fanpro_smc_read(&g_smc, fanpro_fourcc("F0Tg"), &v),
	          FANPRO_SMC_SUCCESS);
	TT_NEAR(v.num, 2200.0, 0.001);

	teardown();
}

TT_TEST(smc_not_privileged_is_a_transport_failure)
{
	setup(FANPRO_FAKE_NOT_PRIVILEGED, 1, false);

	/* Unprivileged writes fail below the firmware, so this must come back
	 * negative rather than as a result code. */
	TT_ASSERT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0Tg"), 1500.0) < 0);
	/* Reads still work without root. */
	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("F0Ac"),
	                              &(fanpro_smc_key_info_t){ 0 }),
	          FANPRO_SMC_SUCCESS);

	teardown();
}

TT_TEST(smc_lowercase_mode_key_generation)
{
	fanpro_smc_key_info_t info;

	/* M5 (Mac17,7) uses F%dmd and has no Ftst.  Untested on real hardware
	 * here; covered only by this fake. */
	setup(FANPRO_FAKE_DIRECT_OK, 1, true);

	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("F0md"), &info),
	          FANPRO_SMC_SUCCESS);
	TT_EQ_INT(fanpro_smc_key_info(&g_smc, fanpro_fourcc("F0Md"), &info),
	          FANPRO_SMC_NOT_FOUND);
	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("F0md"), 1.0),
	          FANPRO_SMC_SUCCESS);

	teardown();
}

TT_TEST(smc_key_enumeration)
{
	uint32_t count = 0, key = 0;

	setup(FANPRO_FAKE_DIRECT_OK, 2, false);

	TT_EQ_INT(fanpro_smc_key_count(&g_smc, &count), FANPRO_SMC_SUCCESS);
	TT_ASSERT(count > 2);

	/*
	 * The contract is that every index in [0, count) yields a key that can
	 * be read back, not that any particular key sits at index 0.  Asserting
	 * a specific slot would pin the fake's insertion order rather than
	 * anything the real firmware promises.
	 */
	{
		uint32_t i;
		int readable = 0;

		for (i = 0; i < count; i++) {
			fanpro_smc_value_t v;

			TT_EQ_INT(fanpro_smc_key_at(&g_smc, i, &key),
			          FANPRO_SMC_SUCCESS);
			TT_ASSERT(key != 0);
			if (fanpro_smc_read(&g_smc, key, &v) == FANPRO_SMC_SUCCESS)
				readable++;
		}
		TT_EQ_UINT((uint32_t)readable, count);
	}

	teardown();
}

TT_TEST(smc_write_to_absent_key_reports_not_found)
{
	setup(FANPRO_FAKE_DIRECT_OK, 1, false);

	TT_EQ_INT(fanpro_smc_write_num(&g_smc, fanpro_fourcc("ZZZZ"), 1.0),
	          FANPRO_SMC_NOT_FOUND);
	TT_EQ_UINT(fanpro_fake_write_count(g_fake, "F0Tg"), 0);

	teardown();
}
