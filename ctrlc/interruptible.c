static const char interruptible_doc[] =
    "C extension demonstrating how to write C extensions that run\n"
    "lengthy calculations while remaining interruptible by control-C\n"
    "or equivalent.";

// Copyright 2024, 2025 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _XOPEN_SOURCE 700
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdbool.h>
#include <signal.h>

#include "kissfft_subset.h"

// 1e9 nanoseconds in a second
#define NS_PER_S (1000 * 1000 * 1000)

// Internally, we work with times as 64-bit counts of nanoseconds.
// 2**64 ns is 584.55 years and we use CLOCK_MONOTONIC so we needn't
// worry about this overflowing.
typedef uint64_t nanosec;

static inline nanosec
monotonic_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (nanosec)now.tv_sec * NS_PER_S + (nanosec)now.tv_nsec;
}

#ifdef CLOCK_MONOTONIC_COARSE
static inline nanosec
monotonic_coarse_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    return (nanosec)now.tv_sec * NS_PER_S + (nanosec)now.tv_nsec;
}
#endif

// The Python convention is to express time intervals as double-
// precision floating-point seconds.  When scaled to seconds, an IEEE
// double can accurately represent all intervals less than 48.5 days
// to nanosecond precision, so this convention should not cause any
// rounding error in this application.
//
// (48.5 days is a lower bound.  The exact point at which IEEE double
// can no longer provide nanosecond precision is somewhere between
// 4194304000000000 ns ≈ 48.5 days and 2⁵³ ns ≈ 103 days.  Because
// 1e-9 is not exactly representable as a binary fraction, and the
// gap between 4194304000000000 and 2⁵³ is on the order of 2⁵², it
// is difficult to pin down the exact break point.)
static inline double
nsec_to_sec(nanosec ns)
{
    return (double)ns * 1e-9;
}

static inline nanosec
sec_to_nsec(double s)
{
    if (s <= 0)
        return 0;
    return (nanosec)llrint(s * 1e9);
}

// Utilities for working with our custom KeyboardInterrupt subclass

static int
define_Interrupted(PyObject *mod)
{
    PyObject *Interrupted = PyErr_NewExceptionWithDoc(
        "interruptible.Interrupted",
        "One of the functions of this module was interrupted by control-C.\n"
        "\n"
        "The `args` property is a 2-tuple (elapsed, checks).\n"
        "This is the same 2-tuple that would have been returned if the\n"
        "calculation had not been interrupted; see\n"
        "`interruptible.fft_uninterruptible`'s docstring for details.\n"
        "\n"
        "This is a subclass of KeyboardInterrupt and therefore will *not*\n"
        "be caught by `except Exception`.",
        PyExc_KeyboardInterrupt,
        0
    );
    if (PyModule_AddObjectRef(mod, "Interrupted", Interrupted) < 0) {
        Py_DECREF(Interrupted);
        return -1;
    }
    *(PyObject **)PyModule_GetState(mod) = Interrupted;
    return 0;
}

static PyObject *
raise_Interrupted(PyObject *mod, PyObject *args)
{
    PyObject *Interrupted = *(PyObject **)PyModule_GetState(mod);

    // Discard the original KeyboardInterrupt exception.
    // Doing "raise new_exception(...) from old_exception" in the
    // C API is more trouble than it's worth.  See discussion here:
    // https://stackoverflow.com/questions/51030659
    // https://github.com/python/cpython/issues/67377
    PyErr_Clear();

    PyErr_SetObject(Interrupted, args);
    Py_DECREF(args);
    return NULL;
}

// `kiss_fft_periodic_cb` poor man's subclass that carries all the
// information required by the different checking mechanisms.

typedef struct periodic_signal_check {
    kiss_fft_periodic_cb base;
    nanosec ns_last_check;
    nanosec ns_between_checks;
    nanosec check_count;
    bool release_gil;
} periodic_signal_check;

// This version of the stop callback doesn't check for signals.
static int
uninterruptible_check(kiss_fft_periodic_cb *payload)
{
    return 0;
}

// This version of the stop callback checks for signals every time
// it is called, which may have significant overhead due to the need
// to claim and release the GIL every time.
static int
simple_interruptible_check(kiss_fft_periodic_cb *payload)
{
    int rv;
    periodic_signal_check *self = (periodic_signal_check *)payload;
    self->check_count += 1;

    // PyErr_CheckSignals requires the GIL.  If self->release_gil is
    // set, we need to re-acquire the GIL in order to call it.
    if (self->release_gil) {
        PyGILState_STATE s = PyGILState_Ensure();
        rv = PyErr_CheckSignals();
        PyGILState_Release(s);
    } else {
        rv = PyErr_CheckSignals();
    }
    return rv;
}

// This version of the stop callback checks for signals only if at
// least 'ns_between_checks' nanoseconds of CLOCK_MONOTONIC time has
// elapsed since the last time signals were checked for, thus avoiding
// claiming and releasing the GIL quite so often.  The price is
// having to call `clock_gettime` on every call.
static int
timed_interruptible_check(kiss_fft_periodic_cb *payload)
{
    periodic_signal_check *self = (periodic_signal_check *)payload;

    nanosec now_ns = monotonic_now_ns();
    if (now_ns - self->ns_last_check < self->ns_between_checks)
        return 0;

    self->ns_last_check = now_ns;
    return simple_interruptible_check(payload);
}

#ifdef CLOCK_MONOTONIC_COARSE
// This is the same as `timed_interruptible_check` just above, except
// it uses CLOCK_MONOTONIC_COARSE.
static int
timed_coarse_interruptible_check(kiss_fft_periodic_cb *payload)
{
    periodic_signal_check *self = (periodic_signal_check *)payload;

    nanosec now_ns = monotonic_coarse_now_ns();
    if (now_ns - self->ns_last_check < self->ns_between_checks)
        return 0;

    self->ns_last_check = now_ns;
    return simple_interruptible_check(payload);
}
#endif

// Shared implementation for all four public functions.

static Py_ssize_t
maybe_interruptible_get_buffers(PyObject *o1, Py_buffer *b1,
                                PyObject *o2, Py_buffer *b2)
{
    if (PyObject_GetBuffer(o1, b1, PyBUF_SIMPLE) < 0) {
        return (Py_ssize_t)-1;
    }

    if (PyObject_GetBuffer(o2, b2, PyBUF_SIMPLE) < 0) {
        PyBuffer_Release(b1);
        return (Py_ssize_t)-1;
    }

    if (b1->len != b2->len) {
        PyErr_SetString(PyExc_ValueError,
                        "input and output must be same size");
        goto fail;
    }

    size_t samples = (size_t) b1->len / sizeof(kiss_fft_cpx);
    if (samples == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "not enough samples: have 0 need 1");
        goto fail;
    }

    if (samples > KISS_FFT_MAX_SAMPLES) {
        PyErr_Format(PyExc_ValueError,
                     "too many samples: have %zd limit %zd",
                     samples, KISS_FFT_MAX_SAMPLES);
        goto fail;
    }

    return (Py_ssize_t) samples;

 fail:
    PyBuffer_Release(b2);
    PyBuffer_Release(b1);
    return (Py_ssize_t) -1;
}


static PyObject *
maybe_interruptible(PyObject *mod, PyObject *td, PyObject *fd,
                    periodic_signal_check *should_stop)
{
    PyObject *res = 0;
    int interrupted = 0;

    Py_buffer tb, fb;
    Py_ssize_t samples = maybe_interruptible_get_buffers(td, &tb, fd, &fb);
    if (samples == (Py_ssize_t) -1) {
        return 0;
    }

    // benchmark.py blocks SIGINT around latency tests, expecting us
    // to unblock it again, so that it can only be delivered during
    // execution of this function; without this we can get stray
    // KeyboardInterrupts
    sigset_t sigint, prev;
    sigemptyset(&sigint);
    sigaddset(&sigint, SIGINT);
    sigprocmask(SIG_UNBLOCK, &sigint, &prev);

    // start timing at this point because kiss_fft_alloc itself may take
    // significant time
    nanosec start_ns = monotonic_now_ns();
    if (should_stop->ns_between_checks)
        should_stop->ns_last_check = start_ns;

    kiss_fft_periodic_cb *ssbase = &should_stop->base;
    kiss_fft_state *st = kiss_fft_alloc((uint32_t) samples);
    if (st == 0) {
        PyErr_NoMemory();
        goto out;
    }
    if (st == (kiss_fft_state *)-1) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid number of samples for KISS FFT"
                        " (not a power of two?)");
        goto out;
    }

    // kiss_fft_alloc itself may take significant time
    if (ssbase->check(ssbase)) {
        interrupted = 1;
    } else if (should_stop->release_gil) {
        Py_BEGIN_ALLOW_THREADS
        interrupted =
            kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf,
                     ssbase);
        Py_END_ALLOW_THREADS
    } else {
        interrupted =
            kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf,
                     ssbase);
    }
    nanosec stop_ns = monotonic_now_ns();

    free(st);

    // Unconditionally check for signals at this point so that,
    // if there's a pending signal, we throw our special Interrupted
    // exception instead of letting the interpreter throw a regular
    // KeyboardInterrupt.
    if (!interrupted && PyErr_CheckSignals())
        interrupted = 1;

    sigprocmask(SIG_SETMASK, &prev, NULL);

    res = Py_BuildValue("dL",
                        nsec_to_sec(stop_ns - start_ns),
                        should_stop ? should_stop->check_count : 0);
    if (interrupted)
        res = raise_Interrupted(mod, res);
 out:
    PyBuffer_Release(&fb);
    PyBuffer_Release(&tb);
    return res;
}

// Parsed arguments for all the functions callable from Python.
struct interruptible_args
{
    PyObject *td;
    PyObject *fd;
    double s_between_checks;
    bool release_gil;
};

static int
parse_interruptible_args(struct interruptible_args *parsed,
                         PyObject *args, PyObject *kwargs)
{
    static const char *const keywords[] = {
        "input", "output", "interval", "release_gil", NULL
    };

    parsed->td = NULL;
    parsed->fd = NULL;
    parsed->s_between_checks = 0.005;  // 5 ms
    parsed->release_gil = true;
    int release_gil = 1;  // 'p' format expects an int

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|dp", (char **)keywords,
                                     &parsed->td,
                                     &parsed->fd,
                                     &parsed->s_between_checks,
                                     &release_gil))
        return 0;

    parsed->release_gil = release_gil; // convert to bool
    return 1;
}

static PyObject *
uninterruptible(PyObject *self, PyObject *args, PyObject *kwargs)
{
    struct interruptible_args parsed;
    if (!parse_interruptible_args(&parsed, args, kwargs))
        return 0;

    struct periodic_signal_check should_stop;
    should_stop.base.check = uninterruptible_check;
    should_stop.check_count = 0;
    should_stop.ns_last_check = 0;
    should_stop.ns_between_checks = 0;
    should_stop.release_gil = parsed.release_gil;

    return maybe_interruptible(self, parsed.td, parsed.fd, &should_stop);
}

static PyObject *
simple_interruptible(PyObject *self, PyObject *args, PyObject *kwargs)
{
    struct interruptible_args parsed;
    if (!parse_interruptible_args(&parsed, args, kwargs))
        return 0;

    struct periodic_signal_check should_stop;
    should_stop.base.check = simple_interruptible_check;
    should_stop.check_count = 0;
    should_stop.ns_last_check = 0;
    should_stop.ns_between_checks = 0;
    should_stop.release_gil = parsed.release_gil;

    return maybe_interruptible(self, parsed.td, parsed.fd, &should_stop);
}

static PyObject *
timed_interruptible(PyObject *self, PyObject *args, PyObject *kwargs)
{
    struct interruptible_args parsed;
    if (!parse_interruptible_args(&parsed, args, kwargs))
        return 0;

    struct periodic_signal_check should_stop;
    should_stop.base.check = timed_interruptible_check;
    should_stop.check_count = 0;
    should_stop.ns_last_check = 0;
    should_stop.ns_between_checks = sec_to_nsec(parsed.s_between_checks);
    should_stop.release_gil = parsed.release_gil;

    return maybe_interruptible(self, parsed.td, parsed.fd, &should_stop);
}

#ifdef CLOCK_MONOTONIC_COARSE
static PyObject *
timed_coarse_interruptible(PyObject *self, PyObject *args, PyObject *kwargs)
{
    struct interruptible_args parsed;
    if (!parse_interruptible_args(&parsed, args, kwargs))
        return 0;

    struct periodic_signal_check should_stop;
    should_stop.base.check = timed_coarse_interruptible_check;
    should_stop.check_count = 0;
    should_stop.ns_last_check = 0;
    should_stop.ns_between_checks = sec_to_nsec(parsed.s_between_checks);
    should_stop.release_gil = parsed.release_gil;

    return maybe_interruptible(self, parsed.td, parsed.fd, &should_stop);
}
#endif


static PyMethodDef interruptible_methods[] = {
    { "fft_uninterruptible",
      (PyCFunction)uninterruptible,
      METH_VARARGS | METH_KEYWORDS,
      "fft_uninterruptible(input, output, interval=0.005, release_gil=True)\n"
      "    -> (elapsed, checks)"
      "\n\n"
      "Performs a Fourier transform, without taking special care to be\n"
      " interruptible by control-C."
      "\n\n"
      "`input` and `output` are the input and output arrays; both must\n"
      "be interpretable as 'simple buffers' of single-precision complex\n"
      "numbers, both must have the same number of elements, and that number\n"
      " must be a power of two.  (NumPy arrays can meet these requirements.)"
      "\n\n"
      "`interval` is the desired interval between checks for control-C,\n"
      "defaulting to 0.005 s (5 ms).  (This function ignores this argument.)"
      "\n\n"
      "If `release_gil` is true, the GIL will be released during the\n"
      "computation of the Fourier transform."
      "\n\n"
      "On success, returns a 2-tuple (elapsed, checks); elapsed is\n"
      "the elapsed time for the calculation, as a floating-point number\n"
      "of seconds, and checks is the number of times that a manual check\n"
      "for control-C was performed (always zero for this function)."
    },
    { "fft_simple_interruptible",
      (PyCFunction)simple_interruptible,
      METH_VARARGS | METH_KEYWORDS,
      "fft_simple_interruptible(input, output, interval=0.005, release_gil=True)\n"
      "    -> (elapsed, checks)"
      "\n\n"
      "Performs a Fourier transform, checking for control-C at convenient\n"
      "points within the transform algorithm.  Arguments and return value\n"
      "are the same as for `fft_uninterruptible`, except that the `checks`\n"
      "element of the return value won't always be zero."
    },
    { "fft_timed_interruptible",
      (PyCFunction)timed_interruptible,
      METH_VARARGS | METH_KEYWORDS,
      "fft_timed_interruptible(input, output, interval=0.005, release_gil=True)\n"
      "    -> (elapsed, checks)"
      "\n\n"
      "Performs a Fourier transform, checking for control-C at convenient\n"
      "points within the transform algorithm, but only if at least\n"
      "the interval specified by the third argument has elapsed since\n"
      "the previous check.  Other arguments are the same as for\n"
      "`fft_uninterruptible`, as is the return value, except that the\n"
      "`checks` element won't always be zero."
    },
#ifdef CLOCK_MONOTONIC_COARSE
    { "fft_timed_coarse_interruptible",
      (PyCFunction)timed_coarse_interruptible,
      METH_VARARGS | METH_KEYWORDS,
      "fft_timed_coarse_interruptible(input, output, interval=0.005, release_gil=True)\n"
      "    -> (elapsed, checks)"
      "\n\n"
      "Same as fft_timed_interruptible but uses a clock with coarser"
      " resolution. It may therefore have lower overhead."
    },
#endif
    { 0, 0, 0, 0 },
};

static struct PyModuleDef interruptible_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "interruptible",
    .m_doc = interruptible_doc,
    .m_size = sizeof(PyObject *), // our KeyboardInterrupt subclass
    .m_methods = interruptible_methods,
};

// called via dlsym; pacify -Wmissing-prototypes
extern PyMODINIT_FUNC PyInit_interruptible(void);

PyMODINIT_FUNC
PyInit_interruptible(void)
{
    PyObject *mod = PyModule_Create(&interruptible_module);
    if (!mod)
        return NULL;

    if (define_Interrupted(mod) < 0) {
        Py_DECREF(mod);
        return NULL;
    }

    if (PyModule_AddIntConstant(mod, "MAX_SAMPLES", KISS_FFT_MAX_SAMPLES)) {
        Py_DECREF(mod);
        return NULL;
    }

    return mod;
}
