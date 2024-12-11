static const char interruptible_doc[] =
    "C extension demonstrating how to write C extensions that"
    " run lengthy calculations while remaining interruptible by"
    " control-C or equivalent.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyObject *
non_interruptible(PyObject *self, PyObject *args)
{
    PyErr_SetNone(PyExc_NotImplementedError);
    return 0;
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
      " interruptible by control-C." },
    { "simple_interruptible", simple_interruptible, METH_VARARGS,
      "Runs a lengthy calculation, checking for control-C"
      " every so many iterations." },
    { "timed_interruptible", timed_interruptible, METH_VARARGS,
      "Runs a lengthy calculation, checking for control-C"
      " every so many milliseconds." },
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
