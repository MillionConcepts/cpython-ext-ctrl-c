"""Statistical benchmark for the code in interruptible.c."""

# Copyright 2024 Million Concepts LLC
# BSD-3-Clause License
# See LICENSE.md for details

import csv
import sys
import time
import numpy as np

from .signaler import PeriodicSignalContext
from .interruptible import (
    fft_uninterruptible,
    fft_simple_interruptible,
    fft_timed_interruptible,
    Interrupted
)

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
            (elapsed, checks) = fft_impl(td, fd)
            yield (sz, rep, elapsed)

        sz *= 2

def measure_ki_latency(fft_impl, ki_delay, interval, size_min, reps, rng):
    """Measure how quickly FFT_IMPL abandons its work upon receipt of
       KeyboardInterrupt.  KI_DELAY is how long to wait after starting
       the FFT before sending the KeyboardInterrupt, INTERVAL is how
       often the FFT should check for interruptions (some impls don't
       honor this option), SIZE_MIN is the number of samples to feed
       the FFT (will be rounded up to the next power of two), REPS is
       how many repetitions to run, and RNG is a numpy random number
       generator.

       Yields tuples (size, delay, interval, rep, interrupted, latency, checks)
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
        try:
            with interrupter:
                (elapsed, checks) = fft_impl(td, fd, interval)
        except Interrupted as e:
            interrupted = True
            (elapsed, checks) = e.args
        stop = time.monotonic()
        yield (sz, ki_delay, interval, rep,
               interrupted, elapsed - ki_delay, checks)


def main():
    with sys.stdout as ofp:
        rng = np.random.default_rng()
        #fft = np.fft.fft

        wr = csv.writer(ofp, dialect='unix', quoting=csv.QUOTE_MINIMAL)

        # wr.writerow(("impl", "size", "rep", "runtime"))
        # for impl in [
        #         non_interruptible,
        #         simple_interruptible,
        #         timed_interruptible
        # ]:
        #     for row in measure_runtime(
        #             impl,
        #             1 << 16,
        #             1 << 24,
        #             20,
        #             rng
        #     ):
        #         wr.writerow((impl.__name__,) + row)
        #         sys.stderr.write(".")
        #         sys.stderr.flush()

        wr.writerow(("impl", "size", "delay", "interval",
                     "rep", "interrupted", "latency", "checks"))
        size = 1 << 21 # takes ~0.1 s if not interrupted
        reps = 30
        for impl in [
#                non_interruptible,
                fft_simple_interruptible,
                fft_timed_interruptible
        ]:
            name = impl.__name__.removeprefix("fft_").removesuffix("_interruptible")
            for delay_raw in range(1, 10):
                delay = delay_raw / 100
                for interval in [0.001, 0.005, 0.01, 0.05, 0.1]:
                    for row in measure_ki_latency(
                            impl,
                            delay,
                            interval,
                            size,
                            reps,
                            rng,
                    ):
                        wr.writerow((name,) + row)
                        sys.stderr.write(".")
                        sys.stderr.flush()
    sys.stderr.write("\n")

if __name__ == "__main__":
    main()
