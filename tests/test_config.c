/*
 * Config tests.
 *
 * Weighted heavily toward rejection.  A parser that skips what it does not
 * understand would let a typo leave a fan bound to nothing, or a curve with
 * reversed points silently return the wrong RPM, and on this hardware the
 * firmware will not correct either mistake.
 */
#include "tinytest.h"

#include "fanpro/config.h"

#include <math.h>
#include <string.h>

static char g_err[FANPRO_CONFIG_ERR_MAX];

static int
parse(fanpro_config_t *cfg, const char *text)
{
	g_err[0] = '\0';
	return fanpro_config_parse(cfg, text, g_err, sizeof(g_err));
}

TT_TEST(config_defaults_are_monitor_only)
{
	fanpro_config_t cfg;

	fanpro_config_defaults(&cfg);

	/* Taking control removes the firmware's protection from that fan, so
	 * doing nothing must be what an unconfigured install does. */
	TT_EQ_INT(cfg.mode, FANPRO_MODE_AUTO);
	TT_EQ_INT(cfg.n_curves, 0);
	TT_FALSE(cfg.safety.allow_fan_stop);
	TT_EQ_STR(cfg.fan_curve[0], "");
	TT_NEAR(cfg.safety.panic_c[FANPRO_CLASS_SOC], 95.0, 0.001);
	TT_NEAR(cfg.safety.panic_c[FANPRO_CLASS_NAND], 80.0, 0.001);
}

TT_TEST(config_parses_a_full_file)
{
	fanpro_config_t cfg;
	const fanpro_curve_t *c;

	TT_EQ_INT(parse(&cfg,
	    "[general]\n"
	    "mode = curve\n"
	    "log_level = debug\n"
	    "panic_nand_c = 75\n"
	    "deadband_rpm = 40\n"
	    "\n"
	    "[fan.0]\n"
	    "curve = safe\n"
	    "\n"
	    "[curve.safe]\n"
	    "source = class:soc\n"
	    "points = 50:1000, 70:2000, 85:max\n"
	    "hysteresis_c = 3\n"
	    "slew_up_rpm = 500\n"
	    "\n"
	    "[alert.hot]\n"
	    "source = class:soc\n"
	    "above_c = 85\n"
	    "notify = true\n"), 0);

	TT_EQ_INT(cfg.mode, FANPRO_MODE_CURVE);
	TT_EQ_INT(cfg.log_level, FANPRO_LOG_DEBUG);
	TT_NEAR(cfg.safety.panic_c[FANPRO_CLASS_NAND], 75.0, 0.001);
	TT_NEAR(cfg.deadband_rpm, 40.0, 0.001);

	c = fanpro_config_curve_for_fan(&cfg, 0);
	TT_ASSERT(c != NULL);
	TT_EQ_STR(c->name, "safe");
	TT_EQ_INT(c->n_points, 3);
	TT_TRUE(c->use_class);
	TT_EQ_INT(c->cls, FANPRO_CLASS_SOC);
	TT_NEAR(c->points[0].celsius, 50.0, 0.001);
	TT_NEAR(c->points[1].rpm, 2000.0, 0.001);
	TT_TRUE(c->points[2].rpm_is_max);
	TT_NEAR(c->hysteresis_c, 3.0, 0.001);

	/* An unbound fan stays on auto rather than inheriting anything. */
	TT_ASSERT(fanpro_config_curve_for_fan(&cfg, 1) == NULL);

	TT_EQ_INT(cfg.n_alerts, 1);
	TT_NEAR(cfg.alerts[0].above_c, 85.0, 0.001);
	TT_TRUE(cfg.alerts[0].notify);
}

TT_TEST(config_rejects_a_fan_bound_to_a_missing_curve)
{
	fanpro_config_t cfg;

	/* The important one: a typo here would otherwise leave the fan on
	 * whatever the defaults happen to be, which nobody chose. */
	TT_ASSERT(parse(&cfg,
	    "[fan.0]\n"
	    "curve = typo\n"
	    "[curve.safe]\n"
	    "points = 50:1000\n") != 0);
	TT_ASSERT(strstr(g_err, "typo") != NULL);
}

TT_TEST(config_rejects_unknown_keys_and_sections)
{
	fanpro_config_t cfg;

	TT_ASSERT(parse(&cfg, "[general]\nnonsense = 1\n") != 0);
	TT_ASSERT(strstr(g_err, "nonsense") != NULL);
	TT_ASSERT(parse(&cfg, "[wat]\nx = 1\n") != 0);
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 50:1000\nbogus = 2\n") != 0);
	/* A key before any section has no meaning. */
	TT_ASSERT(parse(&cfg, "mode = curve\n") != 0);
}

TT_TEST(config_rejects_malformed_points)
{
	fanpro_config_t cfg;

	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 50\n") != 0);
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = abc:1000\n") != 0);
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 50:abc\n") != 0);
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints =\n") != 0);

	/* Out of order would make interpolation silently wrong rather than
	 * fail, which is the worst possible outcome for a fan curve. */
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 70:2000, 50:1000\n") != 0);
	TT_ASSERT(strstr(g_err, "increase") != NULL);
	/* Duplicate temperatures are equally ambiguous. */
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 50:1000, 50:2000\n") != 0);
}

TT_TEST(config_rejects_out_of_range_values)
{
	fanpro_config_t cfg;

	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 500:1000\n") != 0);
	TT_ASSERT(parse(&cfg, "[curve.a]\npoints = 50:99999\n") != 0);
	TT_ASSERT(parse(&cfg, "[general]\npanic_soc_c = 500\n") != 0);
	TT_ASSERT(parse(&cfg, "[general]\ntick_ms = 5\n") != 0);
	TT_ASSERT(parse(&cfg, "[general]\nheartbeat_timeout_s = 1\n") != 0);
	TT_ASSERT(parse(&cfg, "[general]\nmode = sideways\n") != 0);
	TT_ASSERT(parse(&cfg, "[general]\nallow_fan_stop = maybe\n") != 0);
	TT_ASSERT(parse(&cfg, "[fan.99]\ncurve = a\n") != 0);
}

TT_TEST(config_rejects_a_curve_with_no_points)
{
	fanpro_config_t cfg;

	/* A named but empty curve would evaluate to NAN forever, which the
	 * safety gate reads as "release", so the fan would never be driven and
	 * the operator would have no idea why. */
	TT_ASSERT(parse(&cfg, "[curve.a]\nhysteresis_c = 2\n") != 0);
	TT_ASSERT(strstr(g_err, "no points") != NULL);
}

TT_TEST(config_rejects_duplicate_curve_names)
{
	fanpro_config_t cfg;

	TT_ASSERT(parse(&cfg,
	    "[curve.a]\npoints = 50:1000\n"
	    "[curve.a]\npoints = 60:2000\n") != 0);
	TT_ASSERT(strstr(g_err, "duplicate") != NULL);
}

TT_TEST(config_rejects_an_alert_without_a_threshold)
{
	fanpro_config_t cfg;

	TT_ASSERT(parse(&cfg, "[alert.hot]\nsource = class:soc\n") != 0);
	TT_ASSERT(strstr(g_err, "above_c") != NULL);
}

TT_TEST(config_failure_leaves_the_previous_config_intact)
{
	fanpro_config_t cfg;

	TT_EQ_INT(parse(&cfg,
	    "[general]\nmode = curve\n"
	    "[fan.0]\ncurve = good\n"
	    "[curve.good]\npoints = 50:1200\n"), 0);
	TT_EQ_INT(cfg.mode, FANPRO_MODE_CURVE);

	/* A failed reload must not disturb a running daemon's settings. */
	TT_ASSERT(parse(&cfg, "[general]\nmode = nonsense\n") != 0);
	TT_EQ_INT(cfg.mode, FANPRO_MODE_CURVE);
	TT_ASSERT(fanpro_config_curve_for_fan(&cfg, 0) != NULL);
}

TT_TEST(config_comments_blanks_and_whitespace)
{
	fanpro_config_t cfg;

	TT_EQ_INT(parse(&cfg,
	    "# a comment\n"
	    "; another\n"
	    "\n"
	    "   [general]   \n"
	    "   mode   =   curve   \n"
	    "\n"
	    "[curve.a]\n"
	    "  points  =  50:1000 ,  70:2000  \n"), 0);
	TT_EQ_INT(cfg.mode, FANPRO_MODE_CURVE);
	TT_EQ_INT(cfg.curves[0].n_points, 2);
	TT_NEAR(cfg.curves[0].points[1].celsius, 70.0, 0.001);
}

TT_TEST(config_source_selectors)
{
	fanpro_config_t cfg;

	TT_EQ_INT(parse(&cfg,
	    "[curve.a]\nsource = match:tdie\naggregate = avg\npoints = 50:1000\n"), 0);
	TT_FALSE(cfg.curves[0].use_class);
	TT_EQ_STR(cfg.curves[0].match, "tdie");
	TT_EQ_INT(cfg.curves[0].agg, FANPRO_AGG_AVG);

	/* A bare word is a class, for the common case. */
	TT_EQ_INT(parse(&cfg, "[curve.a]\nsource = nand\npoints = 50:1000\n"), 0);
	TT_TRUE(cfg.curves[0].use_class);
	TT_EQ_INT(cfg.curves[0].cls, FANPRO_CLASS_NAND);

	TT_ASSERT(parse(&cfg, "[curve.a]\nsource = class:wat\npoints = 50:1000\n") != 0);
}

TT_TEST(config_missing_file_yields_safe_defaults)
{
	fanpro_config_t cfg;
	char err[FANPRO_CONFIG_ERR_MAX];

	/* A fresh install with no config must monitor, not guess. */
	TT_EQ_INT(fanpro_config_load(&cfg, "/nonexistent/fanpro.conf", err,
	                             sizeof(err)), 0);
	TT_EQ_INT(cfg.mode, FANPRO_MODE_AUTO);
	TT_EQ_INT(cfg.n_curves, 0);
}
