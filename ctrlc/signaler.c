static const char signaler_doc[] =
    "C extension which generates signals from a thread"
    " independent of the CPython GIL.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyObject *
send_sigints(PyObject *self, PyObject *args)
{
    PyErr_SetNone(PyExc_NotImplementedError);
    return 0;
}

static PyMethodDef signaler_methods[] = {
    { "send_sigints", send_sigints, METH_VARARGS,
      "Spawn a thread independent of the GIL that will send SIGINT"
      " signals to the main thread at intervals until stopped."
      " It returns a context manager; to stop the SIGINT thread,"
      " exit the `with` block for that context manager." },
    { 0, 0, 0, 0 },
};

static struct PyModuleDef signaler_module = {
    PyModuleDef_HEAD_INIT,
    "signaler",
    signaler_doc,
    0,
    signaler_methods,
};

PyMODINIT_FUNC
PyInit_signaler(void)
{
    return PyModule_Create(&signaler_module);
}
