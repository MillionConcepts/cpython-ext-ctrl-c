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

void kiss_fft(kiss_fft_state *restrict st,
              const kiss_fft_cpx *restrict fin,
              kiss_fft_cpx *restrict fout);

#endif
