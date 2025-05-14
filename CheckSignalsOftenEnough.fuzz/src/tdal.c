// This file contains both versions of timespec_difference_at_least,
// broken out into separate functions.  It must be kept in sync with
// CheckSignalsOftenEnough.c in the top level of the repository.

#include <stdint.h>
#include <time.h>

#if __GNUC__ >= 3 || (defined __has_builtin && __has_builtin(__builtin_expect))
#  define LIKELY(expr)   __builtin_expect((expr), 1)
#  define UNLIKELY(expr) __builtin_expect((expr), 0)
#else
#  define LIKELY(expr)   (expr)
#  define UNLIKELY(expr) (expr)
#endif

#define ONE_S_IN_NS  INT64_C(1000000000)

/* True if (after - before) >= minimum or (after - before) < 0.
 * `minimum` must be less than ONE_S_IN_NS.
 *
 * This is the straightforward implementation using multiplication.
 */
int c_timespec_difference_at_least_mul(
    const struct timespec *after,
    const struct timespec *before,
    uint_least32_t min_ns // assumed < ONE_S_IN_NS
) {
    int_least64_t before_ns =
        before->tv_sec * ONE_S_IN_NS + before->tv_nsec;
    int_least64_t after_ns =
        after->tv_sec * ONE_S_IN_NS + after->tv_nsec;

    int_least64_t delta_ns = after_ns - before_ns;
    return delta_ns < 0 || delta_ns >= (int_least64_t)min_ns;
}

/* True if (after - before) >= minimum or (after - before) < 0.
 * `minimum` must be no more than ONE_S_IN_NS.
 *
 * This is the sophisticated implementation that avoids multiplication
 * by breaking the comparison down into cases and relying on `minimum`
 * to be less than one second (in nanoseconds).
 */
int c_timespec_difference_at_least_cases(
    const struct timespec *after,
    const struct timespec *before,
    uint_least32_t min_ns // assumed < ONE_S_IN_NS
) {
    // The most probable situation is that after and before
    // are different points within the same second.  In this case
    // we can directly compare the tv_nsec fields.
    if (LIKELY(after->tv_sec == before->tv_sec)) {
        return
            after->tv_nsec - before->tv_nsec >= (int_least64_t)min_ns
            || UNLIKELY(after->tv_nsec < before->tv_nsec);
    }
    // The next most probable situation is that before->tv_sec and
    // after->tv_sec are consecutive.  In this case the result is
    // still determined by the nsec fields, but we need to adjust
    // after->tv_nsec upward by one second's worth of nanoseconds
    // before we can subtract before->tv_nsec.  The result of the
    // subtraction cannot be negative.
    if (LIKELY(after->tv_sec == before->tv_sec + 1)) {
        return (ONE_S_IN_NS + after->tv_nsec) - before->tv_nsec
            >= (int_least64_t)min_ns;
    }
    // The remaining (unlikely) possibilities are:
    //   after->tv_sec > before->tv_sec + 1, in which case the time
    //   difference must be greater than whatever min_ns is
    //   after->tv_sec < before->tv_sec, in which case after < before
    //   no matter what their tv_nsec values are
    return 1;
}
