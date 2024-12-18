static const char interruptible_doc[] =
    "C extension demonstrating how to write C extensions that"
    " run lengthy calculations while remaining interruptible by"
    " control-C or equivalent.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _XOPEN_SOURCE 700
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "kissfft_subset.h"

// 1e9 nanoseconds in a second
#define NS_PER_S (1000 * 1000 * 1000)

// 1e6 nanoseconds in a millisecond
#define NS_PER_MS (1000 * 1000)

static PyObject *
maybe_interruptible(PyObject *self, PyObject *args,
                    kiss_fft_periodic_cb *should_stop)
{
    PyObject *td, *fd, *res = 0;
    Py_buffer tb, fb;

    if (!PyArg_ParseTuple(args, "OO", &td, &fd))
        goto out;
    if (PyObject_GetBuffer(td, &tb, PyBUF_SIMPLE))
        goto out;
    if (PyObject_GetBuffer(fd, &fb, PyBUF_SIMPLE))
        goto out_tb;

    if (tb.len != fb.len) {
        PyErr_SetString(PyExc_ValueError, "input and output must be same size");
        goto out_bb;
    }

    size_t samples = (size_t) tb.len / sizeof(kiss_fft_cpx);
    if (samples > KISS_FFT_MAX_SAMPLES) {
        PyErr_SetString(PyExc_ValueError, "too many samples for KISS FFT");
        goto out_bb;
    }

    kiss_fft_state *st = kiss_fft_alloc((uint32_t) samples);
    if (st == 0) {
        PyErr_NoMemory();
        goto out_bb;
    }
    if (st == (kiss_fft_state *)-1) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid number of samples for KISS FFT"
                        " (not a power of two?)");
        goto out_bb;
    }

    // kiss_fft_alloc itself may take significant time
    if (should_stop && should_stop->check(should_stop)) {
        goto out_st;
    }

    {
        int rv;
        Py_BEGIN_ALLOW_THREADS
        rv = kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf,
                      should_stop);
        Py_END_ALLOW_THREADS
        if (rv)
            goto out_st;
    }

    Py_INCREF(fd);
    res = fd;

 out_st:
    free(st);
 out_bb:
    PyBuffer_Release(&fb);
 out_tb:
    PyBuffer_Release(&tb);
 out:
    return res;
}


static PyObject *
non_interruptible(PyObject *self, PyObject *args)
{
    return maybe_interruptible(self, args, 0);
}

// This version of the stop callback checks for signals every time
// it is called, which may have significant overhead due to the need
// to claim and release the GIL every time.
static int
simple_interruptible_check(kiss_fft_periodic_cb *unused)
{
    // This function may be called either with or without the GIL
    // held.  PyErr_CheckSignals requires the GIL.
    PyGILState_STATE s = PyGILState_Ensure();
    int rv = PyErr_CheckSignals();
    PyGILState_Release(s);
    return rv;
}

static struct kiss_fft_periodic_cb simple_should_stop = {
    simple_interruptible_check
};

static PyObject *
simple_interruptible(PyObject *self, PyObject *args)
{
    return maybe_interruptible(self, args, &simple_should_stop);
}


// This version of the stop callback checks for signals only if
// at least one millisecond of CLOCK_MONOTONIC time has elapsed
// since the last time signals were checked for, thus avoiding
// claiming and releasing the GIL quite so often.
typedef struct timed_periodic_cb {
    kiss_fft_periodic_cb base;
    struct timespec last_signal_check;
} timed_periodic_cb;

static int
timed_interruptible_check(kiss_fft_periodic_cb *statep)
{
    timed_periodic_cb *state = (timed_periodic_cb *)statep;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed_ns =
        (now.tv_sec - state->last_signal_check.tv_sec) * NS_PER_S
        + (now.tv_nsec - state->last_signal_check.tv_nsec);

    if (elapsed_ns > NS_PER_MS) {
        state->last_signal_check = now;
        return simple_interruptible_check(statep);
    }
    return 0;
}


static PyObject *
timed_interruptible(PyObject *self, PyObject *args)
{
    struct timed_periodic_cb should_stop;
    should_stop.base.check = timed_interruptible_check;
    clock_gettime(CLOCK_MONOTONIC, &should_stop.last_signal_check);

    return maybe_interruptible(self, args,
                               (kiss_fft_periodic_cb *)&should_stop);
}

static PyMethodDef interruptible_methods[] = {
    { "non_interruptible", non_interruptible, METH_VARARGS,
      "Runs a lengthy calculation without taking special care to be"
      " interruptible by control-C.  Takes one argument, a buffer of"
      " bits to be analyzed for their randomness.  On success, returns"
      " a 2-tuple of the test statistic and the time elapsed during"
      " the calculation." },
    { "simple_interruptible", simple_interruptible, METH_VARARGS,
      "Runs a lengthy calculation, checking for control-C"
      " every so many iterations.  Arguments and return value are"
      " the same as for non_interruptible." },
    { "timed_interruptible", timed_interruptible, METH_VARARGS,
      "Runs a lengthy calculation, checking for control-C"
      " every so many milliseconds.  Arguments and return value are"
      " the same as for non_interruptible." },
    { 0, 0, 0, 0 },
};

static struct PyModuleDef interruptible_module = {
    PyModuleDef_HEAD_INIT,
    "interruptible",
    interruptible_doc,
    0,
    interruptible_methods,
};

// called via dlsym; pacify -Wmissing-prototypes
extern PyMODINIT_FUNC PyInit_interruptible(void);

PyMODINIT_FUNC
PyInit_interruptible(void)
{
    return PyModule_Create(&interruptible_module);
}
