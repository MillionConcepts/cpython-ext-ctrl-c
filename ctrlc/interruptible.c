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

static PyObject *
non_interruptible(PyObject *self, PyObject *args)
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

    Py_BEGIN_ALLOW_THREADS
    kiss_fft(st, (kiss_fft_cpx *)tb.buf, (kiss_fft_cpx *)fb.buf);
    Py_END_ALLOW_THREADS

    free(st);
    Py_INCREF(fd);
    res = fd;
 out_bb:
    PyBuffer_Release(&fb);
 out_tb:
    PyBuffer_Release(&tb);
 out:
    return res;
}

static PyObject *
simple_interruptible(PyObject *self, PyObject *args)
{
    PyErr_SetNone(PyExc_NotImplementedError);
    return 0;
}

static PyObject *
timed_interruptible(PyObject *self, PyObject *args)
{
    PyErr_SetNone(PyExc_NotImplementedError);
    return 0;
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

PyMODINIT_FUNC
PyInit_interruptible(void)
{
    return PyModule_Create(&interruptible_module);
}
