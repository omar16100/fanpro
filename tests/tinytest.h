/*
 * tinytest - a ~100 line assert harness so `make test` needs no dependency.
 *
 * Usage:
 *   TT_TEST(name) { TT_ASSERT(cond); }        in test_foo.c
 *   TT_RUN(name);                             in test_main.c
 */
#ifndef FANPRO_TINYTEST_H
#define FANPRO_TINYTEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int tt_checks;
extern int tt_failures;
extern int tt_case_failures;
extern const char *tt_current;

/* TT_TEST emits its own prototype so test files stay clean under
 * -Wmissing-prototypes without each case needing a separate declaration. */
#define TT_TEST(name) void tt_case_##name(void); void tt_case_##name(void)
#define TT_DECL(name) void tt_case_##name(void)

#define TT_RUN(name)                                                          \
	do {                                                                  \
		tt_current = #name;                                           \
		tt_case_failures = 0;                                         \
		tt_case_##name();                                             \
		if (tt_case_failures == 0)                                    \
			printf("  ok   %s\n", #name);                         \
		else                                                          \
			printf("  FAIL %s (%d)\n", #name, tt_case_failures);  \
	} while (0)

#define TT__FAIL(fmt, ...)                                                    \
	do {                                                                  \
		tt_failures++;                                                \
		tt_case_failures++;                                           \
		fprintf(stderr, "  %s:%d: %s: " fmt "\n", __FILE__, __LINE__, \
		        tt_current, __VA_ARGS__);                             \
	} while (0)

#define TT_ASSERT(cond)                                                       \
	do {                                                                  \
		tt_checks++;                                                  \
		if (!(cond))                                                  \
			TT__FAIL("assertion failed: %s", #cond);              \
	} while (0)

#define TT_ASSERT_MSG(cond, msg)                                              \
	do {                                                                  \
		tt_checks++;                                                  \
		if (!(cond))                                                  \
			TT__FAIL("%s (%s)", (msg), #cond);                    \
	} while (0)

#define TT_EQ_INT(got, want)                                                  \
	do {                                                                  \
		long long g_ = (long long)(got), w_ = (long long)(want);       \
		tt_checks++;                                                  \
		if (g_ != w_)                                                 \
			TT__FAIL("%s: got %lld, want %lld", #got, g_, w_);     \
	} while (0)

#define TT_EQ_UINT(got, want)                                                 \
	do {                                                                  \
		unsigned long long g_ = (unsigned long long)(got);            \
		unsigned long long w_ = (unsigned long long)(want);           \
		tt_checks++;                                                  \
		if (g_ != w_)                                                 \
			TT__FAIL("%s: got 0x%llx, want 0x%llx", #got, g_, w_); \
	} while (0)

#define TT_EQ_STR(got, want)                                                  \
	do {                                                                  \
		const char *g_ = (got), *w_ = (want);                         \
		tt_checks++;                                                  \
		if (g_ == NULL || w_ == NULL || strcmp(g_, w_) != 0)          \
			TT__FAIL("%s: got \"%s\", want \"%s\"", #got,         \
			         g_ ? g_ : "(null)", w_ ? w_ : "(null)");     \
	} while (0)

#define TT_NEAR(got, want, tol)                                               \
	do {                                                                  \
		double g_ = (double)(got), w_ = (double)(want);               \
		double t_ = (double)(tol);                                    \
		tt_checks++;                                                  \
		if (!(fabs(g_ - w_) <= t_))                                   \
			TT__FAIL("%s: got %.6f, want %.6f (tol %.6f)", #got,  \
			         g_, w_, t_);                                 \
	} while (0)

#define TT_TRUE(cond)  TT_ASSERT(cond)
#define TT_FALSE(cond) TT_ASSERT(!(cond))

#endif /* FANPRO_TINYTEST_H */
