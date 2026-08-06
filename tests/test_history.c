/*
 * History formatting and alert hysteresis.
 *
 * The alert state machine gets the most attention here: an alert that fires
 * on every tick it is over threshold produces a notification per second,
 * which trains the user to ignore all of them, including the one that
 * matters.
 */
#include "tinytest.h"

#include "fanpro/history.h"

#include <math.h>
#include <string.h>

TT_TEST(history_format_is_valid_jsonl)
{
	fanpro_history_row_t row;
	char buf[1024];
	int n;

	memset(&row, 0, sizeof(row));
	row.unix_time = 1770000000;
	row.soc_c = 55.25;
	row.gpu_c = 48.5;
	row.nand_c = 42.0;
	row.cpu_w = 12.5;
	row.gpu_w = 3.25;
	row.n_fans = 2;
	row.fan_rpm[0] = 1500.0;
	row.fan_target[0] = 1480.0;
	row.fan_mode[0] = FANPRO_FAN_MODE_MANUAL;
	row.fan_rpm[1] = 1010.0;
	row.fan_target[1] = 1000.0;
	row.fan_mode[1] = FANPRO_FAN_MODE_AUTO;
	row.thermal_level = 10;
	row.driving = true;

	n = fanpro_history_format(&row, buf, sizeof(buf));
	TT_ASSERT(n > 0);

	/* One line, no embedded newline: the whole point of JSONL. */
	TT_ASSERT(strchr(buf, '\n') == NULL);
	TT_EQ_INT((int)strlen(buf), n);
	TT_ASSERT(buf[0] == '{');
	TT_ASSERT(buf[n - 1] == '}');
	TT_ASSERT(strstr(buf, "\"t\":1770000000") != NULL);
	TT_ASSERT(strstr(buf, "\"soc_c\":55.25") != NULL);
	TT_ASSERT(strstr(buf, "\"rpm\":1500") != NULL);
	TT_ASSERT(strstr(buf, "\"driving\":true") != NULL);
}

TT_TEST(history_missing_readings_are_null_not_zero)
{
	fanpro_history_row_t row;
	char buf[1024];

	memset(&row, 0, sizeof(row));
	row.unix_time = 1;
	row.soc_c = 50.0;
	row.gpu_c = NAN;   /* no GPU sensor on this machine */
	row.nand_c = NAN;
	row.cpu_w = NAN;
	row.gpu_w = NAN;
	row.n_fans = 0;

	TT_ASSERT(fanpro_history_format(&row, buf, sizeof(buf)) > 0);

	/* null, never 0.  A zero would read as a very cold machine to anything
	 * plotting this later, which is worse than an admitted gap. */
	TT_ASSERT(strstr(buf, "\"gpu_c\":null") != NULL);
	TT_ASSERT(strstr(buf, "\"gpu_c\":0") == NULL);
	TT_ASSERT(strstr(buf, "\"cpu_w\":null") != NULL);
}

TT_TEST(history_format_refuses_to_overflow)
{
	fanpro_history_row_t row;
	char tiny[16];

	memset(&row, 0, sizeof(row));
	row.n_fans = FANPRO_MAX_FANS;

	/* Truncating a JSON line would produce a corrupt record that breaks
	 * every reader from that point on. */
	TT_ASSERT(fanpro_history_format(&row, tiny, sizeof(tiny)) < 0);
	TT_ASSERT(fanpro_history_format(&row, NULL, 100) < 0);
	TT_ASSERT(fanpro_history_format(NULL, tiny, sizeof(tiny)) < 0);
}

TT_TEST(history_rotates_on_a_day_boundary)
{
	TT_FALSE(fanpro_history_should_rotate(100, 100));
	TT_TRUE(fanpro_history_should_rotate(100, 101));
	/* New year: day 365 to day 0 is still a boundary. */
	TT_TRUE(fanpro_history_should_rotate(364, 0));
	/* Before the file has been opened there is nothing to rotate. */
	TT_FALSE(fanpro_history_should_rotate(-1, 5));
}

/* ---- alert hysteresis --------------------------------------------------- */

TT_TEST(alert_fires_once_on_crossing)
{
	fanpro_alert_state_t st;
	double t = 1000.0;

	memset(&st, 0, sizeof(st));

	TT_FALSE(fanpro_alert_should_fire(80.0, 85.0, &st, t));
	TT_TRUE(fanpro_alert_should_fire(86.0, 85.0, &st, t + 1));

	/* Still over: silent.  Firing every tick is how an alert becomes
	 * something the user filters out. */
	TT_FALSE(fanpro_alert_should_fire(87.0, 85.0, &st, t + 2));
	TT_FALSE(fanpro_alert_should_fire(90.0, 85.0, &st, t + 3));
	TT_EQ_INT(st.fire_count, 1);
}

TT_TEST(alert_repeats_only_after_a_long_gap)
{
	fanpro_alert_state_t st;
	double t = 1000.0;

	memset(&st, 0, sizeof(st));
	TT_TRUE(fanpro_alert_should_fire(90.0, 85.0, &st, t));

	/* A sustained problem should not go silent forever, but it must not
	 * chatter either. */
	TT_FALSE(fanpro_alert_should_fire(90.0, 85.0, &st,
	                                  t + FANPRO_ALERT_REPEAT_S - 1.0));
	TT_TRUE(fanpro_alert_should_fire(90.0, 85.0, &st,
	                                 t + FANPRO_ALERT_REPEAT_S));
	TT_EQ_INT(st.fire_count, 2);
}

TT_TEST(alert_rearms_only_after_falling_clear)
{
	fanpro_alert_state_t st;
	double t = 1000.0;

	memset(&st, 0, sizeof(st));
	TT_TRUE(fanpro_alert_should_fire(90.0, 85.0, &st, t));

	/* Dipping just below the line must NOT re-arm, or a value hovering on
	 * the threshold fires again every time it wobbles across. */
	TT_FALSE(fanpro_alert_should_fire(84.5, 85.0, &st, t + 1));
	TT_TRUE(st.firing);
	TT_FALSE(fanpro_alert_should_fire(86.0, 85.0, &st, t + 2));
	TT_EQ_INT(st.fire_count, 1);

	/* Comfortably clear re-arms it. */
	TT_FALSE(fanpro_alert_should_fire(85.0 - FANPRO_ALERT_CLEAR_MARGIN_C, 85.0,
	                                  &st, t + 3));
	TT_FALSE(st.firing);

	/* And crossing again is a genuine new event. */
	TT_TRUE(fanpro_alert_should_fire(86.0, 85.0, &st, t + 4));
	TT_EQ_INT(st.fire_count, 2);
}

TT_TEST(alert_ignores_unusable_values)
{
	fanpro_alert_state_t st;

	memset(&st, 0, sizeof(st));

	/* NAN means the sensor class does not exist on this machine.  Treating
	 * that as "not over threshold" is right; treating it as a crossing
	 * would alert constantly on hardware that simply has no GPU sensor. */
	TT_FALSE(fanpro_alert_should_fire(NAN, 85.0, &st, 1.0));
	TT_FALSE(fanpro_alert_should_fire(90.0, NAN, &st, 1.0));
	TT_FALSE(fanpro_alert_should_fire(90.0, 85.0, NULL, 1.0));
	TT_EQ_INT(st.fire_count, 0);
}

TT_TEST(alert_rules_evaluate_independently)
{
	fanpro_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_alert_state_t states[FANPRO_MAX_ALERTS];
	char err[FANPRO_CONFIG_ERR_MAX];

	memset(states, 0, sizeof(states));
	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 90.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "NAND CH0 temp", 45.0, FANPRO_SRC_HID);

	TT_EQ_INT(fanpro_config_parse(&cfg,
	    "[alert.hot_soc]\nsource = class:soc\nabove_c = 85\n"
	    "[alert.hot_nand]\nsource = class:nand\nabove_c = 80\n",
	    err, sizeof(err)), 0);

	fanpro_alerts_evaluate(&cfg, &set, states, 1000.0);

	/* One rule over, one under: they must not share state. */
	TT_TRUE(states[0].firing);
	TT_EQ_INT(states[0].fire_count, 1);
	TT_FALSE(states[1].firing);
	TT_EQ_INT(states[1].fire_count, 0);
}

TT_TEST(history_format_truncation_never_overflows)
{
	fanpro_history_row_t row;
	char buf[512];
	size_t cap;

	memset(&row, 0, sizeof(row));
	row.unix_time = 1770000000;
	row.soc_c = 55.0;
	row.gpu_c = 44.0;
	row.nand_c = 42.0;
	row.cpu_w = 10.0;
	row.gpu_w = 5.0;
	row.n_fans = FANPRO_MAX_FANS;

	/*
	 * Walk every buffer size from tiny to comfortable.  The original code
	 * checked only the first numeric field for truncation, so at sizes
	 * that fit the scalars but not the fans array, `off` ran past the
	 * buffer and the next write computed a huge size_t length and wrote
	 * out of bounds.  Any size must either produce a complete line or
	 * refuse.
	 */
	for (cap = 1; cap <= sizeof(buf); cap++) {
		char guard[512 + 16];
		int n;

		memset(guard, 0x5a, sizeof(guard));
		n = fanpro_history_format(&row, guard, cap);

		if (n >= 0) {
			TT_ASSERT((size_t)n < cap);
			TT_EQ_INT((int)strlen(guard), n);
			TT_ASSERT(guard[n] == '\0');
			TT_ASSERT(guard[0] == '{' && guard[n - 1] == '}');
		}
		/* Nothing beyond the caller's buffer may have been touched. */
		TT_ASSERT((unsigned char)guard[cap] == 0x5a);
		TT_ASSERT((unsigned char)guard[sizeof(guard) - 1] == 0x5a);
	}
}
