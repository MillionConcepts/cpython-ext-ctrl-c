/*
 *  Copyright (c) 2003-2010, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

// For purpose of this demo, KISS FFT has been cut down to only the
// code absolutely required: forward FFT on 32-bit float, max 2**31
// samples, sample size must be a power of two.

#include "kissfft_subset.h"

#include <stdbool.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
// far too many digits on purpose
#define M_PI 3.141592653589793238462643383279502884197169399375105820974944
#endif

// ZW: I can think of several clever ways to pack this information
// into less space given its extreme regularity, or even compute it on
// the fly from the sample size -- it's always a sequence of (4, n)
// where 2 <= n <= 0x2000_0000, followed by either (4, 1) or (2, 1) --
// but I don't feel like debugging any of them.
struct kiss_fft_factor {
    uint32_t radix;
    uint32_t stride;
};

// ZW: 2**31 samples requires 16 factors.  All supported smaller sample
// sizes require fewer factors.  See kf_factor.
#define MAXFACTORS ((size_t)16)

struct kiss_fft_state {
    struct kiss_fft_factor factors[MAXFACTORS];
    kiss_fft_cpx twiddles[];  // actual size [samples - 1]
};

/*
  Explanation of macros dealing with complex math:

   C_MUL(res, a, b)   : res = a*b
   C_SUB(res, a, b)   : res = a - b
   C_SUBFROM(res, a)  : res -= a
   C_ADDTO(res, a)    : res += a
 */

#define C_MUL(res, a, b)                        \
    do {                                        \
        (res).r = (a).r*(b).r - (a).i*(b).i;    \
        (res).i = (a).r*(b).i + (a).i*(b).r;    \
    } while (0)

#define C_ADD(res, a, b)                        \
    do {                                        \
        (res).r = (a).r + (b).r;                \
        (res).i = (a).i + (b).i;                \
    } while (0)

#define C_SUB(res, a, b)                        \
    do {                                        \
        (res).r = (a).r-(b).r;                  \
        (res).i = (a).i-(b).i;                  \
    } while (0)

#define C_ADDTO(res, a)                         \
    do {                                        \
        (res).r += (a).r;                       \
        (res).i += (a).i;                       \
    } while (0)

#define C_SUBFROM(res, a)                       \
    do {                                        \
        (res).r -= (a).r;                       \
        (res).i -= (a).i;                       \
    } while (0)

static void
kf_bfly2(kiss_fft_cpx *Fout,
         const size_t fstride,
         const kiss_fft_state *st,
         uint32_t m)
{
    kiss_fft_cpx *Fout2;
    const kiss_fft_cpx *tw1 = st->twiddles;
    kiss_fft_cpx t;
    Fout2 = Fout + m;
    do {
        C_MUL(t, *Fout2, *tw1);
        tw1 += fstride;
        C_SUB(*Fout2, *Fout, t);
        C_ADDTO(*Fout, t);
        ++Fout2;
        ++Fout;
    } while (--m);
}

static void
kf_bfly4(kiss_fft_cpx *Fout,
         const size_t fstride,
         const kiss_fft_state *st,
         const size_t m)
{
    const kiss_fft_cpx *tw1, *tw2, *tw3;
    kiss_fft_cpx scratch[6];
    size_t k = m;
    const size_t m2 = 2 * m;
    const size_t m3 = 3 * m;

    tw3 = tw2 = tw1 = st->twiddles;

    do {
        C_MUL(scratch[0], Fout[m], *tw1);
        C_MUL(scratch[1], Fout[m2], *tw2);
        C_MUL(scratch[2], Fout[m3], *tw3);

        C_SUB(scratch[5], *Fout, scratch[1]);
        C_ADDTO(*Fout, scratch[1]);
        C_ADD(scratch[3], scratch[0], scratch[2]);
        C_SUB(scratch[4], scratch[0], scratch[2]);
        C_SUB(Fout[m2], *Fout, scratch[3]);
        tw1 += fstride;
        tw2 += fstride * 2;
        tw3 += fstride * 3;
        C_ADDTO(*Fout, scratch[3]);

        Fout[m].r = scratch[5].r + scratch[4].i;
        Fout[m].i = scratch[5].i - scratch[4].r;
        Fout[m3].r = scratch[5].r - scratch[4].i;
        Fout[m3].i = scratch[5].i + scratch[4].r;

        ++Fout;
    } while (--k);
}

static int
kf_work(kiss_fft_cpx *Fout,
        const kiss_fft_cpx *f,
        const size_t fstride,
        const struct kiss_fft_factor *factors,
        const kiss_fft_state *st,
        kiss_fft_periodic_cb *should_stop)
{
    kiss_fft_cpx *Fout_beg = Fout;
    const uint32_t p = factors->radix;   /* the radix  */
    const uint32_t m = factors->stride;   /* stage's fft length/p */
    const kiss_fft_cpx *Fout_end = Fout + p * m;
    int rv;

    if (m == 1) {
        do {
            *Fout = *f;
            f += fstride;
        } while (++Fout != Fout_end);
    } else {
        do {
            // recursive call:
            // DFT of size m*p performed by doing
            // p instances of smaller DFTs of size m,
            // each one takes a decimated version of the input
            {
                int rv = kf_work(Fout, f, fstride * p, factors + 1, st,
                                 should_stop);
                if (rv) return rv;
            }
            f += fstride;
        } while ((Fout += m) != Fout_end);
    }

    rv = should_stop->check(should_stop);
    if (rv) return rv;

    Fout = Fout_beg;

    // recombine the p smaller DFTs
    switch (p) {
    case 2:
        kf_bfly2(Fout, fstride, st, m);
        break;
    case 4:
        kf_bfly4(Fout, fstride, st, m);
        break;
    default:
        abort();
    }

    return should_stop->check(should_stop);
}

int
kiss_fft(kiss_fft_state *st, const kiss_fft_cpx *fin, kiss_fft_cpx *fout,
         kiss_fft_periodic_cb *should_stop)
{
    return kf_work(fout, fin, 1, st->factors, st, should_stop);
}

/* populate facbuf with { p1, m1 }, { p2, m2 }, ....
    where
    p[i] * m[i] = m[i-1]
    m0 = n                  */
static bool
kf_factor(uint32_t n, struct kiss_fft_factor facbuf[static MAXFACTORS])
{
    // Factor out all powers of 4 first
    size_t i = 0;
    while (n % 4 == 0) {
        if (i >= MAXFACTORS)
            // overrun, should be impossible, handle gracefully anyway
            return false;
        n /= 4;
        facbuf[i].radix = 4;
        facbuf[i].stride = n;
        i++;
    }

    // then remaining powers of 2 (at most one, but we write the loop anyway)
    while (n % 2 == 0) {
        if (i >= MAXFACTORS)
            // overrun, should be impossible, handle gracefully anyway
            return false;
        n /= 2;
        facbuf[i].radix = 2;
        facbuf[i].stride = n;
        i++;
    }

    // fail if input was not a power of two
    return n == 1;
}

/*
 *
 * User-callable function to allocate all necessary storage space for the fft.
 *
 * The return value is a contiguous block of memory, allocated with malloc.  As such,
 * It can be freed with free(), rather than a kiss_fft-specific function.
 * */
kiss_fft_state *kiss_fft_alloc(uint32_t samples)
{
    size_t memneeded = sizeof(struct kiss_fft_state)
        + sizeof(kiss_fft_cpx) * (samples - 1);     /* twiddle factors */

    kiss_fft_state *st = malloc(memneeded);
    if (!st)
        return 0;

    if (!kf_factor(samples, st->factors)) {
        // samples is not a power of two or is too big
        free(st);
        return (kiss_fft_state *)-1;
    }

    for (uint32_t i = 0; i < samples - 1; ++i) {
        double phase = -2.0 * M_PI * i / samples;
        // set st->twiddles[i] = cexp(J * phase) where J is the imaginary unit
        st->twiddles[i].r = cosf((float) phase);
        st->twiddles[i].i = sinf((float) phase);
    }
    return st;
}
