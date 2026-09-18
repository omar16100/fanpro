/*
 * Sensor registry tests.
 *
 * Classification is safety-relevant, not cosmetic: it selects which panic
 * threshold a reading is judged against, so a NAND sensor landing in the SoC
 * class would be judged at 95 C when it should trip at 80 C.
 */
#include "tinytest.h"

#include "fanpro/sensors.h"

#include "fanpro/smc.h"
#include "fanpro/smc_fake.h"

#include <math.h>
#include <string.h>

TT_TEST(sensor_classify_by_pattern)
{
	/* Names measured on this Mac Studio M3 Ultra. */
	TT_EQ_INT(fanpro_sensor_classify("PMU tdie7"), FANPRO_CLASS_SOC);
	TT_EQ_INT(fanpro_sensor_classify("NAND CH0 temp"), FANPRO_CLASS_NAND);
	TT_EQ_INT(fanpro_sensor_classify("PMU tdev6"), FANPRO_CLASS_POWER);
	TT_EQ_INT(fanpro_sensor_classify("PMU tcal"), FANPRO_CLASS_POWER);

	/* Names from other Apple Silicon machines. */
	TT_EQ_INT(fanpro_sensor_classify("SOC MTR Temp Sensor0"), FANPRO_CLASS_SOC);
	TT_EQ_INT(fanpro_sensor_classify("GPU1 Temp Sensor"), FANPRO_CLASS_GPU);
	TT_EQ_INT(fanpro_sensor_classify("pACC MTR Temp Sensor3"), FANPRO_CLASS_SOC);
	TT_EQ_INT(fanpro_sensor_classify("eACC MTR Temp Sensor1"), FANPRO_CLASS_SOC);

	/* Unknown must fall to OTHER, never be guessed into a class with a
	 * lower threshold. */
	TT_EQ_INT(fanpro_sensor_classify("Tf76"), FANPRO_CLASS_OTHER);
	TT_EQ_INT(fanpro_sensor_classify(""), FANPRO_CLASS_OTHER);
	TT_EQ_INT(fanpro_sensor_classify(NULL), FANPRO_CLASS_OTHER);
}

TT_TEST(sensor_classify_nand_wins_over_soc)
{
	/* A name mentioning both must land in the stricter class.  Getting
	 * this backwards means judging an SSD sensor at the SoC threshold. */
	TT_EQ_INT(fanpro_sensor_classify("SOC NAND temp"), FANPRO_CLASS_NAND);
	TT_EQ_INT(fanpro_sensor_classify("ANS CPU sensor"), FANPRO_CLASS_NAND);
}

TT_TEST(sensor_classify_is_case_insensitive)
{
	TT_EQ_INT(fanpro_sensor_classify("nand ch0"), FANPRO_CLASS_NAND);
	TT_EQ_INT(fanpro_sensor_classify("gpu temp"), FANPRO_CLASS_GPU);
	TT_EQ_INT(fanpro_sensor_classify("Soc Die"), FANPRO_CLASS_SOC);
}

TT_TEST(sensor_add_disambiguates_duplicate_names)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));

	/* A multi-die machine reports the same sensor name several times with
	 * genuinely different values; both readings must survive. */
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID));
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie1", 51.0, FANPRO_SRC_HID));
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie1", 52.0, FANPRO_SRC_HID));

	TT_EQ_INT(set.count, 3);
	TT_EQ_STR(set.sensors[0].name, "PMU tdie1");
	TT_EQ_STR(set.sensors[1].name, "PMU tdie1#2");
	TT_EQ_STR(set.sensors[2].name, "PMU tdie1#3");
	TT_NEAR(set.sensors[1].celsius, 51.0, 0.001);
}

TT_TEST(sensor_add_does_not_confuse_prefixes)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));

	/* "PMU tdie1" must not be treated as a duplicate of "PMU tdie10". */
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID));
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie10", 49.0, FANPRO_SRC_HID));
	TT_TRUE(fanpro_sensors_add(&set, "PMU tdie10", 50.0, FANPRO_SRC_HID));

	TT_EQ_STR(set.sensors[1].name, "PMU tdie10");
	TT_EQ_STR(set.sensors[2].name, "PMU tdie10#2");
}

TT_TEST(sensor_add_reports_overflow_instead_of_truncating_silently)
{
	static fanpro_sensor_set_t set; /* too large for the stack */
	int i;

	memset(&set, 0, sizeof(set));

	for (i = 0; i < FANPRO_MAX_SENSORS; i++)
		TT_TRUE(fanpro_sensors_add(&set, "x", 40.0, FANPRO_SRC_HID));

	TT_EQ_INT(set.count, FANPRO_MAX_SENSORS);
	TT_EQ_INT(set.dropped, 0);

	/* Past the cap the add must fail loudly enough for a caller to notice.
	 * A silently truncated set that happens to omit the hottest sensor is
	 * a safety problem, not a cosmetic one. */
	TT_FALSE(fanpro_sensors_add(&set, "y", 99.0, FANPRO_SRC_HID));
	TT_EQ_INT(set.dropped, 1);
	TT_EQ_INT(set.count, FANPRO_MAX_SENSORS);
}

TT_TEST(sensor_add_rejects_empty_names)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	TT_FALSE(fanpro_sensors_add(&set, "", 40.0, FANPRO_SRC_HID));
	TT_FALSE(fanpro_sensors_add(&set, NULL, 40.0, FANPRO_SRC_HID));
	TT_FALSE(fanpro_sensors_add(NULL, "x", 40.0, FANPRO_SRC_HID));
	TT_EQ_INT(set.count, 0);
}

TT_TEST(sensor_aggregation)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "PMU tdie2", 52.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "NAND CH0 temp", 42.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "GPU die", 61.0, FANPRO_SRC_HID);

	TT_NEAR(fanpro_sensors_max(&set), 61.0, 0.001);
	TT_NEAR(fanpro_sensors_max_of_class(&set, FANPRO_CLASS_SOC), 52.0, 0.001);
	TT_NEAR(fanpro_sensors_max_of_class(&set, FANPRO_CLASS_NAND), 42.0, 0.001);
	TT_NEAR(fanpro_sensors_avg_matching(&set, "tdie"), 50.0, 0.001);
}

TT_TEST(sensor_aggregation_of_absent_class_is_nan)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID);

	/* NAN rather than 0.0 matters: a curve that read 0 C for a missing
	 * class would conclude the machine is freezing and idle the fans. */
	TT_TRUE(isnan(fanpro_sensors_max_of_class(&set, FANPRO_CLASS_GPU)));
	TT_TRUE(isnan(fanpro_sensors_avg_matching(&set, "nothing")));
	TT_TRUE(isnan(fanpro_sensors_max(&(fanpro_sensor_set_t){ 0 })));
	TT_TRUE(isnan(fanpro_sensors_max(NULL)));
}

TT_TEST(sensor_aggregation_skips_invalid)
{
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "PMU tdie2", 99.0, FANPRO_SRC_HID);
	set.sensors[1].valid = false; /* last sample failed */

	TT_NEAR(fanpro_sensors_max(&set), 48.0, 0.001);
	TT_NEAR(fanpro_sensors_avg_matching(&set, "tdie"), 48.0, 0.001);
}

/*
 * Ta0* on Mac15,14 returns the flt payload 00 00 20 41, exactly 10.0, on all
 * four keys and on ftA0.  It is a firmware placeholder, not a measurement,
 * and registry.c files Ta0* under the ambient class.  Admitting it turned
 * panic_ambient_c into a guard permanently 60 C clear of its own threshold:
 * strictly worse than an empty class, because safety.c honours a finite
 * reading and only skips a non-finite one.
 */
TT_TEST(smc_temp_rejects_the_firmware_placeholder_reading)
{
	fanpro_fake_t *fake = fanpro_fake_create(FANPRO_FAKE_DIRECT_OK);
	fanpro_smc_t smc;
	fanpro_sensor_set_t set;

	/* The placeholder, and a real reading on the same type and path. */
	fanpro_fake_add_key(fake, "Ta0P", "flt ", 4, 10.0);
	fanpro_fake_add_key(fake, "TB0p", "flt ", 4, 48.125);

	TT_EQ_INT(fanpro_smc_open(&smc, fanpro_fake_backend(fake)), 0);
	memset(&set, 0, sizeof(set));
	TT_TRUE(fanpro_smc_temp_read(&set, &smc) >= 1);

	/* The real one is kept. */
	TT_TRUE(isfinite(fanpro_sensors_max_of_class(&set, FANPRO_CLASS_OTHER)));
	/* The placeholder is dropped, so the ambient class stays empty and
	 * safety.c keeps failing open on it rather than trusting 10 C. */
	TT_TRUE(!isfinite(
	    fanpro_sensors_max_of_class(&set, FANPRO_CLASS_AMBIENT)));

	fanpro_smc_close(&smc);
	fanpro_fake_destroy(fake);
}

/* Pin the classification the placeholder rejection depends on. */
TT_TEST(sensor_classify_puts_ta0_keys_in_the_ambient_class)
{
	TT_EQ_INT(fanpro_sensor_classify("Ta09"), FANPRO_CLASS_AMBIENT);
	TT_EQ_INT(fanpro_sensor_classify("Ta0P"), FANPRO_CLASS_AMBIENT);
}
