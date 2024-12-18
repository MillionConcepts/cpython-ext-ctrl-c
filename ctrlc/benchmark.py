"""Statistical benchmark for the code in interruptible.c."""

# Copyright 2024 Million Concepts LLC
# BSD-3-Clause License
# See LICENSE.md for details

import csv
import sys
import time
import numpy as np

from .signaler import PeriodicSignalContext

def measure_runtime(fft_impl, size_min, size_max, reps, rng):
    """Measure the *total* runtime of FFT implementation FFT_IMPL
       on power-of-two-sized random input arrays such that
       SIZE_MIN <= size < SIZE_MAX.  REPS is how many repetitions
       to run for each size and RNG is a numpy random number generator.

       Yields tuples (size, rep, runtime) for each measurement.
    """
    f32 = np.float32
    c64 = np.complex64
    zeros = np.zeros

    sz = 1
    while sz < size_min:
        sz *= 2

    while sz < size_max:
        # preallocate input and output ndarrays for each size
        tdbuf = np.zeros((sz, 2), dtype=f32)
        td = tdbuf.view(dtype=c64)[..., 0]

        fdbuf = np.zeros((sz, 2), dtype=f32)
        fd = fdbuf.view(dtype=c64)[..., 0]

        for rep in range(reps):
            rng.random((sz, 2), dtype=f32, out=tdbuf)
            start = time.monotonic()
            fft_impl(td, fd)
            stop = time.monotonic()
            yield (sz, rep, stop - start)

        sz *= 2

def measure_ki_latency(fft_impl, ki_delay, size_min, reps, rng):
    """Measure how quickly FFT_IMPL abandons its work upon receipt of
       KeyboardInterrupt.  KI_DELAY is how long to wait after starting
       the FFT before sending the KeyboardInterrupt, SIZE_MIN is the
       number of samples to feed the FFT (will be rounded up to the
       next power of two), REPS is how many repetitions to run, and
       RNG is a numpy random number generator.

       Yields tuples (size, delay, rep, interrupted, latency)
       for each measurement.
    """

    f32 = np.float32
    c64 = np.complex64
    zeros = np.zeros

    sz = 1
    while sz < size_min:
        sz *= 2

    # preallocate input and output ndarrays
    tdbuf = np.zeros((sz, 2), dtype=f32)
    td = tdbuf.view(dtype=c64)[..., 0]

    fdbuf = np.zeros((sz, 2), dtype=f32)
    fd = fdbuf.view(dtype=c64)[..., 0]

    interrupter = PeriodicSignalContext(ki_delay)

    for rep in range(reps):
        rng.random((sz, 2), dtype=f32, out=tdbuf)
        start = time.monotonic()
        interrupted = False
        try:
            with interrupter:
                fft_impl(td, fd)
        except KeyboardInterrupt:
            interrupted = True
        stop = time.monotonic()
        yield (sz, ki_delay, rep, interrupted, (stop - start) - ki_delay)


def main():
    with sys.stdout as ofp:
        wr = csv.writer(ofp, dialect='unix', quoting=csv.QUOTE_MINIMAL)
        wr.writerow(("size", "delay", "rep", "interrupted", "latency"))

        rng = np.random.default_rng()
        fft = np.fft.fft
        for row in measure_ki_latency(
                lambda fd, td: fft(fd, out=td),
                0.005, # 5 ms
                1 << 24,
                144,
                rng,
        ):
            wr.writerow(row)
            sys.stderr.write(".")
            sys.stderr.flush()
    sys.stderr.write("\n")

if __name__ == "__main__":
    main()
