# Handling control-C (`KeyboardInterrupt`) smoothly in CPython C extensions

The CPython documentation warns you that

> A long-running calculation implemented purely in C (such as regular
> expression matching on a large body of text) may run uninterrupted
> for an arbitrary amount of time, regardless of any signals
> received. The Python signal handlers will be called when the
> calculation finishes.
>
> — [`signal` module docs, CPython 3.10][sigquote]

In particular, this means a slow calculation in a C extension won’t
stop when [the user hits an interrupt key][kbdint] unless the C
extension’s authors have taken special care.  The code in this
repository demonstrates how to take that special care.

[`ctrlc/interruptible.c`][interruptible] is the source code for a
CPython extension module that defines several functions callable from
Python.  All of them perform the same “long-running calculation implemented
purely in C”—specifically, they compute the discrete Fourier transform
of an input array.  (This is meant to be a realistic placeholder for
whatever actual calculation you want to do.  Please be aware that
we have made extensive modifications to Fourier transform code
originally written by someone else and it might not be a *correct*
FFT implementation anymore.)

* `fft_uninterruptible` does not take any special care and therefore will
  not be interrupted by control-C.

* `fft_simple_interruptible` checks for signals after each pass of the FFT.

* `fft_timed_interruptible` checks for signals with an adjustable amount
  of wall-clock time between checks.

* `fft_timed_coarse_interruptible` is the same as `fft_timed_interruptible`
  except that it uses a lower-precision clock to reduce overhead.
  This function is only present on systems that provide such a clock
  (specifically, the`CLOCK_MONOTONIC_COARSE` mode of
  [`clock_gettime`][cgettime]).

All of these functions can either release, or not release, Python’s
global interpreter lock during execution.  Releasing the lock
dramatically increases the overhead of checking for signals, as the
lock must be reclaimed to do the check.  The `timed` versions of the
FFT mitigate this extra overhead by only reclaiming the lock once per
interval.

[`ctrlc/benchmark.py`][benchmark] is a statistical benchmark for
the code in `interruptible.c`.

[`ctrlc/signaler.c`][signaler] helps out `benchmark.py` by generating
`SIGINT` signals at periodic intervals.

[`pycon-2025`][pycon] contains slides and notes for a talk about this
project which was presented at [PyCon 2025][].

[`CheckSignalsOftenEnough.c`](CheckSignalsOftenEnough.c) contains a
utility function which automates the process of checking for signals
once per wall-clock interval (hardwired at one millisecond; longer
intervals have no further beneficial effect on overhead), reclaiming
the GIL only when needed.  We hope that this function will soon be
superseded by changes to core CPython (see [gh-133465][]), but until
then, you are encouraged to copy this function into your extensions.

## Licensing

Most of the code is Copyright (c) 2024–2025, Million Concepts LLC.
Some files are taken (with extensive modification) from [KISS FFT][],
which is Copyright (c) 2003-2010, Mark Borgerding.  All of the code
is released to the public under the BSD 3-Clause License; see
[`LICENSE.md`][license] for exact terms.

[sigquote]: https://docs.python.org/3.10/library/signal.html
[kbdint]: https://docs.python.org/3.10/library/exceptions.html#KeyboardInterrupt
[interruptible]: ctrlc/interruptible.c
[cgettime]: https://www.man7.org/linux/man-pages/man3/clock_gettime.3.html
[benchmark]: ctrlc/benchmark.py
[signaler]: ctrlc/signaler.c
[pycon]: pycon-2025/slides.html
[gh-133465]: https://github.com/python/cpython/issues/133465
[license]: LICENSE.md
[KISS FFT]: https://github.com/mborgerding/kissfft
