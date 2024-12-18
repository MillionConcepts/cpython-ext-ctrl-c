/*
 *  Copyright (c) 2003-2010, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

#ifndef KISS_FFT_H
#define KISS_FFT_H

#include <stddef.h>
#include <stdint.h>

#define KISS_FFT_MAX_SAMPLES (UINT32_C(1) << 31)

typedef struct kiss_fft_cpx {
    float r;
    float i;
} kiss_fft_cpx;

typedef struct kiss_fft_state kiss_fft_state;

kiss_fft_state *kiss_fft_alloc(uint32_t samples);

// Added for this demo: if the SHOULD_STOP argument to kiss_fft is not
// NULL, should_stop->check(should_stop) will be called at suitable
// points during execution.  If it returns a nonzero value, kiss_fft
// will abandon its work and return that value as quickly as possible.
// Passing NULL is equivalent to passing a check function that always
// returns zero.
typedef struct kiss_fft_periodic_cb {
    int (*check)(struct kiss_fft_periodic_cb *);
} kiss_fft_periodic_cb;

int kiss_fft(kiss_fft_state *restrict st,
             const kiss_fft_cpx *restrict fin,
             kiss_fft_cpx *restrict fout,
             kiss_fft_periodic_cb *should_stop);

#endif
