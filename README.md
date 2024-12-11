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
CPython extension module that defines three functions callable from
Python.  All three perform “a long-running calculation implemented
purely in C”—specifically, they run a statistical test on a random
number generator.  (This is meant to be a realistic placeholder for
whatever actual calculation you want to do.)  `non_interruptible`
does not take any special care and therefore will not be interrupted
by control-C.  `simple_interruptible` checks for signals every
N iterations of the statistical test, and `timed_interruptible`
checks for signals every K milliseconds of wall-clock time.
This is the code you probably want to read.

[`ctrlc/benchmark.py`][benchmark] is a statistical benchmark for
the code in `interruptible.c`.  When run, it will print summary
statistics to the terminal and record all the raw data in a CSV
file for you to analyze as you see fit.

[`ctrlc/signaler.c`][signaler] helps out `benchmark.py` by generating
`SIGINT` signals from a thread independent of the Python GIL.

## Licensing

Most of the code is Copyright (c) 2024, Million Concepts, and released
to the public under the BSD 3-Clause License.  In particular, this
license applies to all the code in `interruptible.c`, `benchmark.py`,
and `signaler.c`.

A few files are taken from the [TestU01][] library for statistical
testing of random number generators.  These files are under the
Apache License Version 2.0.

See [`LICENSE.md`][license] for further detail.

[sigquote]: https://docs.python.org/3.10/library/signal.html
[kbdint]: https://docs.python.org/3.10/library/exceptions.html#KeyboardInterrupt
[interruptible]: ctrlc/interruptible.c
[benchmark]: ctrlc/benchmark.py
[signaler]: ctrlc/signaler.c
[license]: LICENSE.md
[TestU01]: https://simul.iro.umontreal.ca/testu01/tu01.html
