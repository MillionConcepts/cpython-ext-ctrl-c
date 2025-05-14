/* Copyright (c) 2025, Million Concepts LLC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// This file has been fully tested on Linux and compile-tested on NetBSD,
// using recent but not bleeding edge versions of gcc and clang, against
// C-API headers from Python 3.8 through 3.13.  It uses only stable C-API
// functions, plus the standard POSIX function `clock_gettime`.
//
// If you encounter problems, please send us a bug report via
// <https://github.com/MillionConcepts/cpython-ext-ctrl-c/issues>.

#include <Python.h>
#include <stdint.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC_COARSE
#  define CLOCK_MONOTONIC_COARSE  CLOCK_MONOTONIC
#endif

#if __GNUC__ >= 3 || (defined __has_builtin && __has_builtin(__builtin_expect))
#  define LIKELY(expr)   __builtin_expect((expr), 1)
#  define UNLIKELY(expr) __builtin_expect((expr), 0)
#else
#  define LIKELY(expr)   (expr)
#  define UNLIKELY(expr) (expr)
#endif

#define ONE_MS_IN_NS INT64_C(1000000)
#define ONE_S_IN_NS  INT64_C(1000000000)

/* True if (after - before) >= min_ns or (after - before) < 0.
 * `min_ns` must be less than ONE_S_IN_NS.
 *
 * Two implementations are provided: one is straightforward and easy to
 * validate by eye, the other uses a complicated series of case checks to
 * avoid multiplication.  The straightforward implementation is correct for
 * arbitrarily large min_ns, as long as none of the arithmetic overflows;
 * the clever implementation relies on `min_ns` being less than one second
 * (in nanoseconds).
 *
 * The CheckSignalsOftenEnough.fuzz directory contains a property-based
 * fuzz test that both implementations of this function are equivalent,
 * for all inputs that are valid for both.
 */
static int timespec_difference_at_least(
    const struct timespec *after,
    const struct timespec *before,
    uint_least32_t min_ns
) {
#ifdef SIMPLE_TIMESPEC_DIFFERENCE
    int_least64_t before_ns =
        before->tv_sec * ONE_S_IN_NS + before->tv_nsec;
    int_least64_t after_ns =
        after->tv_sec * ONE_S_IN_NS + after->tv_nsec;

    int_least64_t delta_ns = after_ns - before_ns;
    return delta_ns < 0 || delta_ns >= (int_least64_t)min_ns;

#else
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
#endif
}

extern int CheckSignalsOftenEnough(void) {
    static struct timespec last_check = { 0, 0 };
    struct timespec now;
    PyGILState_STATE st;
    int err;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    if (!timespec_difference_at_least(&now, &last_check, ONE_MS_IN_NS))
        return 0;

    last_check = now;
    st = PyGILState_Ensure();
    err = PyErr_CheckSignals();
    PyGILState_Release(st);
    return err;
}
