"""Statistical benchmark for the code in interruptible.c.

Measures either how efficiently a CPython compiled-code extension
can compute the Fourier transform of a random input vector ('runtime'
mode), or how quickly that extension returns to the interpreter
when interrupted ('latency' mode).  Summary statistics are written
to standard output, and all of the raw data is saved in a CSV file.
"""

# Copyright 2024, 2025 Million Concepts LLC
# BSD-3-Clause License
# See LICENSE.md for details

import argparse
import contextlib
import csv
import signal
import sys
import time

from datetime import timedelta
from io import TextIOBase
from typing import Callable, Iterable

import numpy as np

from .signaler import Timer
from .interruptible import (
    fft_uninterruptible,
    fft_simple_interruptible,
    fft_timed_interruptible,
    Interrupted,
    MAX_SAMPLES,
)


#: Maps command-line algorithm names to implementations and their
#: properties.  Currently the only property is a boolean, "does this
#: algorithm pay attention to its 'interval' argument?".
ALGORITHMS = {
    "none"    : (fft_uninterruptible, False),
    "simple"  : (fft_simple_interruptible, False),
    "fine"    : (fft_timed_interruptible, True),
}
try:
    from .interruptible import fft_timed_coarse_interruptible
    ALGORITHMS["coarse"] = (fft_timed_coarse_interruptible, True)
except ImportError:
    pass
ALL_ALGORITHMS = list(ALGORITHMS.keys())


DEFAULT_INTERVALS = [1., 2., 5., 10.]
DEFAULT_DELAYS = [1., 2., 5., 10., 20., 50., 100.]


START_TIME = None


def verbose_progress(msg, *args):
    global START_TIME
    now = time.monotonic()
    if START_TIME is None:
        START_TIME = now
    elapsed = timedelta(seconds = now - START_TIME)
    msg = msg.format(*args)
    sys.stderr.write(f"[{elapsed}]  {msg}\n")


def quiet_progress(msg, *args):
    pass


def power_of_two_sizes(min: int, max: int) -> Iterable[int]:
    """Yield powers of two starting at MIN up to and including MAX.
       Assumes MIN and MAX are actually powers of two."""
    p = min
    while p <= max:
        yield p
        p *= 2


@contextlib.contextmanager
def mask_signal(sig: int):
    """On entry, block signal SIG for the calling thread.
    On exit, clear any pending instance of SIG and then unblock it."""
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, [sig])
    try:
        yield None
    finally:
        # setting a signal to be ignored has the side effect of
        # clearing any pending instance of that signal, whether or not
        # it's blocked; doing this _before_ unblock means that the
        # signal in question will not actually be delivered.
        signal.signal(sig, signal.signal(sig, signal.SIG_IGN))
        signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)

def alloc_buffers(sz: int) -> (
    np.ndarray, np.ndarray, np.ndarray, np.ndarray
):
    """Preallocate input and output ndarrays for one problem size."""
    tr = np.zeros((sz, 2), dtype=np.float32)
    tc = tr.view(dtype=np.complex64)[..., 0]
    fr = np.zeros((sz, 2), dtype=np.float32)
    fc = tr.view(dtype=np.complex64)[..., 0]
    return (tr, tc, fr, fc)


def bench_runtime(
    data_fp: TextIOBase,
    stats_fp: TextIOBase,
    *,
    summary_stats: bool,
    progress: Callable,
    algorithms: Iterable[str],
    intervals: Iterable[float],
    sizes: Iterable[int],
    repeat: int,
) -> None:
    """Measure the runtime of a set of FFT algorithms on random input
       arrays."""

    rng = np.random.default_rng()
    wr = csv.writer(data_fp, dialect='unix', quoting=csv.QUOTE_MINIMAL)
    wr.writerow(("size", "impl", "interval", "rep", "elapsed", "checks"))
    for size in sizes:
        tr, tc, fr, fc = alloc_buffers(size)
        for alg in algorithms:
            fft_impl, uses_interval = ALGORITHMS[alg]
            if uses_interval:
                ivs = intervals
            else:
                ivs = [0.0]
            for interval in ivs:
                for rep in range(repeat):
                    rng.random(tr.shape, tr.dtype, tr)
                    progress("s={} a={} i={} {}/{}",
                             size, alg, interval, rep + 1, repeat)
                    elapsed, checks = fft_impl(tc, fc, interval)
                    wr.writerow((size, alg, interval, rep + 1,
                                 elapsed, checks))
    progress("done")


def bench_latency(
    data_fp: TextIOBase,
    stats_fp: TextIOBase,
    *,
    summary_stats: bool,
    progress: Callable,
    algorithms: Iterable[str],
    intervals: Iterable[float],
    delays: Iterable[float],
    sizes: Iterable[int],
    repeat: int,
):
    """Measure how quickly each of a set of FFT algorithms will abandon
       its work upon receipt of KeyboardInterrupt."""

    rng = np.random.default_rng()
    wr = csv.writer(data_fp, dialect='unix', quoting=csv.QUOTE_MINIMAL)
    wr.writerow(("size", "impl", "delay", "interval",
                 "rep", "interrupted", "latency", "checks"))

    for size in sizes:
        tr, tc, fr, fc = alloc_buffers(size)
        for delay in delays:
            interrupter = Timer(delay, repeat=False)
            for alg in algorithms:
                fft_impl, uses_interval = ALGORITHMS[alg]
                if uses_interval:
                    ivs = intervals
                else:
                    ivs = [0.0]
                for interval in ivs:
                    for rep in range(repeat):
                        rng.random(tr.shape, tr.dtype, tr)
                        progress("s={} a={} i={} d={} {}/{}",
                                 size, alg, interval, delay, rep + 1, repeat)
                        interrupted = False
                        with mask_signal(signal.SIGINT):
                            try:
                                with interrupter:
                                    (elapsed, checks) = fft_impl(
                                        tc, fc, interval
                                    )
                            except Interrupted as e:
                                interrupted = True
                                (elapsed, checks) = e.args
                        wr.writerow((size, alg, delay, interval, rep,
                                     interrupted, elapsed - delay, checks))
    progress("done")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("runtime", "latency"),
                    help="Measurement mode")
    ap.add_argument("-a", "--algorithm", dest="algorithms", action="append",
                    choices=ALL_ALGORITHMS,
                    help="Algorithm to benchmark (default: all)"
                    " (repeat this option to test several algorithms)")
    ap.add_argument("-o", "--output", default=None,
                    help="Raw benchmarking data is written to this file"
                    " (default: <mode>.csv)")
    ap.add_argument("-q", "--quiet", "--no-summary-stats",
                    dest="summary_stats", action="store_false",
                    help="Don't print summary statistics")
    ap.add_argument("-p", "--progress", action="store_true",
                    help="Report progress of the measurement")

    ap.add_argument("-m", "--min-samples", metavar="SAMPLES",
                    type=int, default = 1 << 16,
                    help="Minimum number of samples to benchmark with."
                    " Must be a power of two.")
    ap.add_argument("-M", "--max-samples", metavar="SAMPLES",
                    type=int, default = 1 << 20,
                    help="Maximum number of samples to benchmark with."
                    f" Must be a power of two and <= {MAX_SAMPLES}.")
    ap.add_argument("-i", "--check-interval", metavar="MS", dest="intervals",
                    type=float, action="append",
                    help="FFT implementation should check for interrupts"
                    " every MS milliseconds (repeat this option to test"
                    " several intervals) (ignored by some algorithms)")
    ap.add_argument("-d", "--interrupt-delay", metavar="MS", dest="delays",
                    type=float, action="append",
                    help="Interrupt the calculation after MS milliseconds"
                    " (repeat this option to test several delays)"
                    " (only meaningful in 'latency' mode)")
    ap.add_argument("-r", "--repeat", metavar="N",
                    type=int, default=5,
                    help="How many times to repeat each measurement.")

    args = ap.parse_args()
    if args.min_samples <= 0:
        ap.error("argument of --min-samples must be positive")
    if args.min_samples.bit_count() != 1:
        ap.error("argument of --min-samples must be a power of two")

    if args.max_samples <= 0:
        ap.error("argument of --max-samples must be positive")
    if args.max_samples.bit_count() != 1:
        ap.error("argument of --max-samples must be a power of two")
    if args.max_samples > MAX_SAMPLES:
        ap.error(f"argument of --max-samples must be <= {MAX_SAMPLES}")

    if args.repeat <= 0:
        ap.error("argument of --repeat must be positive")

    if args.algorithms is None:
        args.algorithms = ALL_ALGORITHMS

    # rescale intervals and delays to 1.0 = 1 second
    intervals = [
        iv * 1e-3
        for iv in
        (DEFAULT_INTERVALS if args.intervals is None else args.intervals)
    ]
    if any(iv <= 0 for iv in intervals):
        ap.error("all arguments of --check-interval must be positive")

    delays = [
        d * 1e-3
        for d in
        (DEFAULT_DELAYS if args.delays is None else args.delays)
    ]
    if any(d <= 0 for d in delays):
        ap.error("all arguments of --interrupt-delay must be positive")

    if args.output is None:
        args.output = args.mode + ".csv"

    if args.progress:
        reporter = verbose_progress
    else:
        reporter = quiet_progress

    with open(args.output, "wt") as data_fp, sys.stdout as stats_fp:
        if args.mode == "runtime":
            bench_runtime(
                data_fp,
                stats_fp,
                summary_stats=args.summary_stats,
                progress=reporter,
                algorithms=args.algorithms,
                intervals=intervals,
                sizes=power_of_two_sizes(args.min_samples, args.max_samples),
                repeat=args.repeat,
            )
        else:
            bench_latency(
                data_fp,
                stats_fp,
                summary_stats=args.summary_stats,
                progress=reporter,
                algorithms=args.algorithms,
                intervals=intervals,
                delays=delays,
                sizes=power_of_two_sizes(args.min_samples, args.max_samples),
                repeat=args.repeat,
            )


if __name__ == "__main__":
    sys.exit(main())
