# Introduction

allow me to introducing myself

# Whadya mean “interruptible”

## Have you ever had this happen?

* you start some complicated computation in NumPy (or whatever)
* you realize you made a mistake
* you press <kbd>control-C</kbd>
* it doesn’t stop running for several seconds

## What went wrong

* Whenever this happens, it’s a bug in a compiled-code extension.
* It didn’t call `PyErr_CheckSignals` often enough.

* This talk is about:
  - Why extensions need to do that
  - Why, today, many extensions *don’t* do that
  - How we can make it easier to do that

## Audience background check

Raise your hand if …

* you’ve written code in a compiled language
  * C specifically. Not Ada, not C++, not D, not Fortran, not Swift,
    not Rust, not Zig, etc etc etc.

* you have written a compiled-code extension for CPython
* you did that using the C-API directly — no intermediate libraries or
  helpers

* you have written a multithreaded program
* you have written a signal handler
* you know the difference between thread-safe code and
  async-signal-safe code

## What happens when you press <kbd>control-C</kbd><sup>1</sup>

* Starts out like any other keystroke
* Reaches the _terminal line discipline_ as ASCII character `'\x03'`
* (by default) Converted to a _signal_, `SIGINT`
* Python interpreter reacts to `SIGINT` by raising `KeyboardInterrupt`
* Normal exception handling process for `KeyboardInterrupt`
  - It’s not a subclass of `Exception`, to discourage people catching
    it too early

## `KeyboardInterrupt` is already trouble for pure Python

- writing code that’s exception safe is already hard
  (`with` statements do help)
- exceptions that can be thrown _at any point_ are worse

> If a signal handler raises an exception, the exception will be
> propagated to the main thread and may be raised after any bytecode
> instruction. Most notably, a KeyboardInterrupt may appear at any
> point during execution. Most Python code, including the standard
> library, cannot be made robust against this, and so a
> KeyboardInterrupt (or any other exception resulting from a signal
> handler) may on rare occasions put the program in an unexpected
> state.

(`signal` module docs)

- as usual, C makes everything harder

## How CPython deals with signals

* C-level signals can be “delivered” after _any machine instruction_
  - Like how `KeyboardInterrupt` can be thrown after any bytecode
    instruction, but nastier

* For safety, Python’s C-level signal handler just sets flags in the
  interpreter state

* The interpreter’s main loop checks those flags periodically
  (right after some — not all — bytecode instructions)

* When those flags are set, it calls the Python-level signal handlers

* The default *Python-level* handler for `SIGINT` throws `KeyboardInterrupt`

## “Delivered”? “Any machine instruction”?

It’s hard to appreciate just how nasty C-level signals are if you
haven’t seen for yourself what they do at the machine level, so I’m
gonna show you.

Here’s a pure Python function that takes a long time to execute.

```python
import random
def f(n=100000000):
    return [random.random() for _ in range(n)]
```

Let’s run this under the _C_ debugger.  I start the interpreter, set
breakpoints on a few functions I know are interesting, and then I let
it run normally…

```
$ gdb /usr/bin/python3.12
[…]
(gdb) break main
Breakpoint 1 at 0x1060: file ./Programs/python.c, line 14.
(gdb) run
Starting program: /usr/bin/python3.12
[...]
Breakpoint 1, main (argc=1, argv=0x7fffffffd8a8) at ./Programs/python.c:14
14	{
(gdb) break signal_handler
Breakpoint 2 at 0x7f948f276a00: file ./Modules/signalmodule.c, line 347.
(gdb) continue
Continuing.
Python 3.12.9 (main, Feb 23 2025, 20:12:24) [GCC 14.2.1 20241221] on linux
Type "help", "copyright", "credits" or "license" for more information.
>>>
```

Now I can type at the Python interpreter as usual.  I define `f()`,
evaluate it, and immediately hit control-C…

```
>>> f()
^C
Program received signal SIGINT, Interrupt.
pymalloc_pool_extend (pool=..., size=1) at Objects/obmalloc.c:1361
1361	        *(pymem_block **)(pool->freeblock) = NULL;
(gdb) signal SIGINT
Continuing with signal SIGINT.

Breakpoint 2, signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
{
(gdb) backtrace
#0  signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
#1  <signal handler called>
#2  pymalloc_pool_extend (pool=..., size=1) at Objects/obmalloc.c:1361
#3  pymalloc_alloc (_unused_ctx=..., state=..., nbytes=32) at Objects/obmalloc.c:1546
#4  _PyObject_Malloc (ctx=..., nbytes=32) at Objects/obmalloc.c:1564
...
```

`pymalloc_alloc` doesn’t contain code to call `signal_handler`, but
somehow, `signal_handler` was called anyway.

This is the black magic of signals.  The operating system has
“preempted” normal execution of the CPython interpreter, at a
completely arbitrary point - happens to be inside pymalloc -
forged two stack frames and adjusted the registers to make the CPU
_behave as if_ `pymalloc_pool_extend` had called `signal_handler`
from that arbitrary point, and then resumed execution of the process.

I need to make you _feel_ this so I’m going to show you the assembly
language.  `signal_handler` itself isn’t special, but…

```
(gdb) up
#1  <signal handler called>
(gdb) disassemble
Dump of assembler code for function __restore_rt:
=> <__restore_rt+0>:	mov    $0xf,%rax  # SYS_rt_sigreturn
   <__restore_rt+7>:	syscall
End of assembler dump.
(gdb) up
#2  pymalloc_pool_extend (pool=..., size=1) at Objects/obmalloc.c:1361
(gdb) disassemble ($pc-6),($pc+7)
Dump of assembler code from 0x7ffff79323b6 to 0x7ffff79323c3:
   <_PyObject_Malloc+470>:	mov    %edi,0x8(%rdx)
   <_PyObject_Malloc+473>:	mov    %ecx,0x28(%rdx)
=> <_PyObject_Malloc+476>:	movq   $0x0,(%rdi)
   <_PyObject_Malloc+483>:	jmp    <_PyObject_Malloc+412>
```

There’s no call instructions!  We’re going to return from the signal
handler to a special C library function, that was never actually
_called_, and from there to a totally arbitrary point inside pymalloc!

Who thinks it’s safe for the CPython interpreter to run the code that
throws KeyboardInterrupt right now?  Literally right now, inside
pymalloc?

It’s not.

And that’s why the C-level signal handler just sets flags in the
interpreter state.

## Signals arriving inside compiled-code extensions

Now let’s do the same thing with NumPy instead.

```python
import numpy as np
rng = np.random.default_rng()
def f(n=1000000000):
    return rng.random(n)
```

I added another zero to `n` because this is quite a bit faster.

Running under the debugger again…

```
>>> f()
^C
Thread 1 "python3" received signal SIGINT, Interrupt.
0x00007ffff00b23a7 in random_standard_uniform_fill ()
   from .../numpy/random/_generator.cpython-312-x86_64-linux-gnu.so
(gdb) signal SIGINT
Continuing with signal SIGINT.

Thread 1 "python3" hit Breakpoint 2, signal_handler (sig_num=2)
    at ./Modules/signalmodule.c:347
347	{
(gdb) backtrace
#0  signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
#1  <signal handler called>
#3  random_standard_uniform_fill ()
#4  __pyx_f_5numpy_6random_7_common_double_fill ()
#5  __pyx_pw_5numpy_6random_10_generator_9Generator_15random ()
#6  method_vectorcall_FASTCALL_KEYWORDS (...) at Objects/descrobject.c:427
#7  _PyObject_VectorcallTstate (...) at ./Include/internal/pycore_call.h:92
#8  PyObject_Vectorcall (...) at Objects/call.c:325
#9  _PyEval_EvalFrameDefault (...) at Python/bytecodes.c:2715
```

The signal handler’s gonna set those flags, but the next _chance_ the
_interpreter_ is gonna get to check them is when we return all the
way to `method_vectorcall_FASTCALL_KEYWORDS`.  And it isn’t _actually_
going to check them until some time after we return to the main
bytecode evaluation loop, in `_PyEval_EvalFrameDefault`.

NumPy _could_ check, by calling `PyErr_CheckSignals`.  _Does_ it?

```
(gdb) break PyErr_CheckSignals
(gdb) continue
… 20 seconds later …

Thread 1 "python3" hit Breakpoint 3, PyErr_CheckSignals ()
    at ./Modules/signalmodule.c:1761
1761	{
(gdb) backtrace
#0  PyErr_CheckSignals () at ./Modules/signalmodule.c:1761
#1  PyObject_Repr (...) at Objects/object.c:544
#2  PyFile_WriteObject (...) at ./Python/sysmodule.c:732
#4  cfunction_vectorcall_O (...) at Objects/methodobject.c:509
#5  _PyObject_VectorcallTstate (...) at ./Include/internal/pycore_call.h:92
#6  PyObject_CallOneArg (func=<sys.displayhook>) at Objects/call.c:401
#7  _PyEval_EvalFrameDefault (...) at Python/bytecodes.c:593
```

It does not.

## Could CPython do anything different?

### No.

* There’s no safe way to throw an exception from inside a
  compiled-code module without that module’s cooperation.
* Just like there’s no safe way to do that from inside pymalloc.

* Even in a compiled language with exceptions, code has to be
  _designed_ to be exception safe.

## Could NumPy do anything different?

### Yes, but…

* Abstractly simple: Call `PyErr_CheckSignals` periodically in code
  that may run for a long time.  If it returns `-1`, cancel what you
  were doing and return to the interpreter as quickly as possible.

* Not obvious from C-API docs that you need to do this

* Cancelling what you were doing may require significant code
  reorganization

* `PyErr_CheckSignals` can run arbitrary Python code
  - Yes, that means you need to hold the GIL
    (I haven’t looked at the free-threading work at all yet)
  - Can’t be cheating on reference counts, etc.
  - Substantial overhead to call it at all, especially if you dropped
    the GIL

## Example of code changes required

### Non-interruptible code

```c
static PyObject *fft(PyObject *td, PyObject *fd)
{
    PyObject *rv = 0;
    kiss_fft_state *st = 0;
    Py_buffer tb, fb;
    Py_ssize_t samples = get_buffers(td, &tb, fd, &fb);
    if (samples == (Py_ssize_t) -1) {
        return 0;
    }
    st = kiss_fft_alloc((uint32_t) samples);
    if (st == 0) {
        PyErr_NoMemory();
        goto out;
    }

    Py_BEGIN_ALLOW_THREADS
    kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf, ssbase);
    Py_END_ALLOW_THREADS

    rv = fd;
    Py_INCREF(rv);
out:
free(st);
    PyBuffer_Release(&fb);
    PyBuffer_Release(&tb);
    return rv;
}
```

### Interruptible code

```c
static PyObject *fft(PyObject *td, PyObject *fd)
{
    int interrupted = 0;
    PyObject *rv = 0;
    kiss_fft_state *st = 0;
    Py_buffer tb, fb;
    Py_ssize_t samples = maybe_interruptible_get_buffers(td, &tb, fd, &fb);
    if (samples == (Py_ssize_t) -1) {
        return 0;
    }
    if (! (st = kiss_fft_alloc((uint32_t) samples)) {
        PyErr_NoMemory();
        goto out;
    }
    if (PyErr_CheckSignals())
        goto out;

    Py_BEGIN_ALLOW_THREADS
    interrupted =
        kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf, ssbase);
    Py_END_ALLOW_THREADS

    if (interrupted)
        goto out;

    rv = fd;
    Py_INCREF(rv);
out:
    free(st);
    PyBuffer_Release(&fb);
    PyBuffer_Release(&tb);
    return rv;
}
```

## That didn’t look so bad, right?

But did you notice `kiss_fft` is now returning a value?  “Interrupted”?

```c
static void
kf_work(kiss_fft_cpx *Fout,
        const kiss_fft_cpx *f,
        const size_t fstride,
        const struct kiss_fft_factor *factors,
        const kiss_fft_state *st)
{
    kiss_fft_cpx *Fout_beg = Fout;
    const uint32_t p = factors->radix;
    const uint32_t m = factors->stride;
    const kiss_fft_cpx *Fout_end = Fout + p * m;

    if (m == 1) {
        do {
            *Fout = *f;
            f += fstride;
        } while (++Fout != Fout_end);
    } else {
        do {
            kf_work(Fout, f, fstride * p, factors + 1, st);
            f += fstride;
        } while ((Fout += m) != Fout_end);
    }

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
}
```

This is the main loop of the FFT implementation.  It’s recursive.
We need to figure out where to put calls to `PyErr_CheckSignals`,
and we need to pass its return value back out…

```c
static int
kf_work(kiss_fft_cpx *Fout,
        const kiss_fft_cpx *f,
        const size_t fstride,
        const struct kiss_fft_factor *factors,
        const kiss_fft_state *st)
{
    kiss_fft_cpx *Fout_beg = Fout;
    const uint32_t p = factors->radix;
    const uint32_t m = factors->stride;
    const kiss_fft_cpx *Fout_end = Fout + p * m;

    if (m == 1) {
        do {
            *Fout = *f;
            f += fstride;
        } while (++Fout != Fout_end);
    } else {
        do {
            int rv = kf_work(Fout, f, fstride * p, factors + 1, st);
            if (rv) return rv;
            f += fstride;
        } while ((Fout += m) != Fout_end);
    }

    {
        PyGILState_STATE s = PyGILState_Ensure();
        int rv = PyErr_CheckSignals();
        PyGILState_Release(s);
        if (rv) return rv;
    }

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

    {
        PyGILState_STATE s = PyGILState_Ensure();
        int rv = PyErr_CheckSignals();
        PyGILState_Release(s);
        if (rv) return rv;
    }

    return 0;
}
```

Those changes are pretty small, but to write them, I had to understand
this particular FFT implementation in _detail_.  And this is a
_simple_ FFT library.  “KISS” stands for “Keep It Simple, Simon.”
Officially.

## Overhead

…

## How can we make this better?

* Ideas I have:
  - Improve core documentation
  - Make `PyErr_CheckSignals` cheaper
  - Tools like Cython and Numba could help out
  - PyO3 could too with creative use of `async`

## Doc improvements

Here’s the documentation for `PyErr_CheckSignals` that I mentioned
earlier.

> `int PyErr_CheckSignals()`
>
>    This function interacts with Python’s signal handling.
>
>    If the function is called from the main thread and under the main Python interpreter, it checks whether a signal has been sent to the processes and if so, invokes the corresponding signal handler. If the `signal` module is supported, this can invoke a signal handler written in Python.
>
>    The function attempts to handle all pending signals, and then returns `0`. However, if a Python signal handler raises an exception, the error indicator is set and the function returns `-1` immediately (such that other pending signals may not have been handled yet: they will be on the next `PyErr_CheckSignals()` invocation).
>
>    If the function is called from a non-main thread, or under a non-main Python interpreter, it does nothing and returns `0`.
>
>    **This function can be called by long-running C code that wants to be interruptible by user requests (such as by pressing Ctrl-C).**

This is buried deep in the C-API docs.  […]


## Speed up `PyErr_CheckSignals`

Specifically:  Make it so you _don’t_ need to hold the GIL to call it.

Right now (3.12) it looks like this (simplified just a little):

```c
int PyErr_CheckSignals(void)
{
    int status = 0;
    PyThreadState *tstate = _PyThreadState_GET();
    if (_Py_atomic_load_relaxed(&tstate->interp->ceval->gc_scheduled)) {
        /* run GC */
    }
    if (_Py_atomic_load(&is_tripped)) {
        status = /* run signal handlers */
    }
}
```

Both “run GC” and “run signal handlers” need the GIL.  But the atomic
loads do _not_.  That’s the whole point of an atomic load.  So let’s
make it be like this instead:

```c
int PyErr_CheckSignals(void)
{
    int status = 0;
    PyThreadState *tstate = _PyThreadState_GET();
    if (_Py_atomic_load_relaxed(&tstate->interp->ceval->gc_scheduled)
        || _Py_atomic_load(&is_tripped)) {
        PyGILState_STATE s = PyGILState_Ensure();
        if (_Py_atomic_load_relaxed(&tstate->interp->ceval->gc_scheduled)) {
            /* run GC */
        }
        if (_Py_atomic_load(&is_tripped)) {
            status = /* run signal handlers */
        }
        PyGILState_Release(s);
    }
    return status;
}
```

With this change, you still need to be _able_ to reclaim the GIL in
order to call `PyErr_CheckSignals`, but you don’t have to do it
yourself.  It will do it for you, only when necessary.

----

on reaction latency:

https://www.tactuallabs.com/papers/howMuchFasterIsFastEnoughCHI15.pdf
https://link.springer.com/chapter/10.1007/978-3-319-58475-1_1
https://dl.gi.de/server/api/core/bitstreams/80cf4901-1392-48a0-a0c8-e653cb0ca021/content

no real consensus. some evidence people can notice delays of 5–10ms
particularly in direct interaction (e.g. drawing on a tablet)

for “stop what you’re doing and give me a command prompt”,
50ms or below is probably a good target
