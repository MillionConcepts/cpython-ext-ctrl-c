static const char signaler_doc[] =
    "C extension which generates signals from a thread"
    " independent of the CPython GIL.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _XOPEN_SOURCE 700
#define PY_SSIZE_T_CLEAN 1
#include <Python.h>
#include <signal.h>

// 1e9 nanoseconds in a second
#define NS_PER_S (1000 * 1000 * 1000)

typedef struct PeriodicSignalContextObject {
    PyObject_HEAD

    // Interval at which to send signals
    struct timespec interval;

    // The number of the signal to be sent.  Zero if this object's
    // __init__ has not yet been called.
    int signal;

    // Depth of calls to __enter__.
    unsigned int entry_count;

    // Timer object.  If __init__ has not yet been called,
    // the value of this field is indeterminate.
    timer_t timer;

} PeriodicSignalContextObject;


// We override tp_new just to make absolutely sure that ->signal is
// zeroed when PSC_init is called for the first time.
static PyObject *
PSC_new(PyTypeObject *type, PyObject *Py_UNUSED(a), PyObject *Py_UNUSED(k))
{
    PeriodicSignalContextObject *self =
        (PeriodicSignalContextObject *) type->tp_alloc(type, 0);
    if (!self)
        return 0;

    self->interval.tv_sec = 0;
    self->interval.tv_nsec = 0;
    self->signal = 0;
    self->entry_count = 0;
    // there is no portable way to initialize a timer_t field other
    // than by calling timer_create, and we can't call timer_create
    // until __init__
    return (PyObject *)self;
}

static int
PSC_init(PyObject *s, PyObject *args, PyObject *kwds)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    if (self->signal) {
        PyErr_SetString(PyExc_RuntimeError,
                        "PeriodicSignalContext.__init__ called twice");
        return -1;
    }

    double interval = 0;
    int signal = SIGINT;
    static char *kwlist[] = {
        "interval", "signal", 0
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                                     "d|i:PeriodicSignalContext", kwlist,
                                     &interval, &signal))
        return -1;

    // This sigaction call will fail if `signal` is not a valid signal
    // number; its only other effect is to write data to `dummy`.
    struct sigaction dummy;
    if (sigaction(signal, 0, &dummy) != 0) {
        PyErr_Format(PyExc_ValueError, "%d is not a valid signal number",
                     signal);
        return -1;
    }

    if (!isfinite(interval) || interval <= 0) {
        PyErr_SetString(PyExc_ValueError,
                        "interval must be positive and finite");
        return -1;
    }
    // The smallest time interval representable by struct timespec is
    // 1 ns = 1e-9 s.
    if (interval < 1e-9) {
        PyErr_SetString(PyExc_ValueError, "minimum interval is 1 ns (1e-9 s)");
        return -1;
    }

    time_t interval_sec = (time_t)floor(interval);
    unsigned long interval_ns =
        (unsigned long)floor((interval - interval_sec) * 1e9);

    self->signal = signal;
    self->interval.tv_sec = interval_sec;
    self->interval.tv_nsec = interval_ns;

    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = signal;
    if (timer_create(CLOCK_MONOTONIC, &sev, &self->timer)) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    return 0;
}

static void
PSC_dealloc(PyObject *s)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    timer_delete(self->timer);
}

static PyObject *
PSC_enter(PyObject *s, PyObject *Py_UNUSED(ignored))
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    if (self->entry_count == UINT_MAX) {
        PyErr_SetString(PyExc_RuntimeError,
            "too many nested calls to PeriodicSignalContext.__enter__");
        return NULL;
    }
    if (self->entry_count == 0) {
        struct itimerspec arm;
        arm.it_value.tv_sec = self->interval.tv_sec;
        arm.it_interval.tv_sec = self->interval.tv_sec;

        arm.it_value.tv_nsec = self->interval.tv_nsec;
        arm.it_interval.tv_nsec = self->interval.tv_nsec;

        if (timer_settime(self->timer, 0, &arm, 0)) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
    }
    self->entry_count += 1;
    return Py_NewRef(self);
}

static PyObject *
PSC_exit(PyObject *s, PyObject *Py_UNUSED(ignored))
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    if (self->entry_count == 1) {
        struct itimerspec disarm;
        memset(&disarm, 0, sizeof disarm);
        timer_settime(self->timer, 0, &disarm, 0);
    }
    if (self->entry_count > 0) {
        self->entry_count -= 1;
    }
    Py_RETURN_NONE;
}

static PyObject *
PSC_get_signal(PyObject *s, void *Py_UNUSED(ignored))
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    return PyLong_FromLong(self->signal);
}

static PyObject *
PSC_get_interval(PyObject *s, void *Py_UNUSED(ignored))
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    return PyFloat_FromDouble(
        self->interval.tv_sec + self->interval.tv_nsec * 1.0e-9
    );
}

static PyMethodDef PSC_methods[] = {
    { "__enter__", PSC_enter, METH_NOARGS,
      "Start sending periodic signals.  See class docs for details." },
    { "__exit__", PSC_exit, METH_VARARGS,
      "Stop sending periodic signals.  See class docs for details." },
    { 0, 0, 0, 0 },
};

static PyGetSetDef PSC_getsetters[] = {
    { "signal", PSC_get_signal, 0,
      "The signal being sent", 0 },
    { "interval", PSC_get_interval, 0,
      "Interval between signals, in milliseconds", 0 },
    { 0, 0, 0, 0, 0 }
};

static PyTypeObject PeriodicSignalContextType = {
    PyVarObject_HEAD_INIT(0, 0)
    .tp_name = "signaler.PeriodicSignalContext",
    .tp_doc = PyDoc_STR(
"with PeriodicSignalContext(0.1, signal=signal.SIGINT):\n"
"   do_stuff_that_gets_interrupted_repeatedly()\n"
"\n"
"Within the context established by using a PeriodicSignalContext object\n"
"in a `with` statement, Unix signals will be sent to the interpreter\n"
"process periodically.\n"
"\n"
"The first (mandatory) argument is the interval at which to\n"
"send signals; like time.sleep, this is in seconds, and may be a\n"
"floating-point number to express an interval shorter than one\n"
"second. The second (optional) argument is the signal to send,\n"
"defaulting to signal.SIGINT.  With the default treatment of\n"
"this signal, that means your program will field periodic\n"
"KeyboardInterrupt exceptions.\n"
"\n"
"Note that Python-level signal handlers are only ever executed on\n"
"the interpreter's \"main thread\" (usually the initial thread of\n"
"the interpreter process).  If you create a PeriodicSignalContext\n"
"object on a different thread, it is still the main thread that\n"
"will field KeyboardInterrupt exceptions.\n"
"\n"
"Also, if several threads are blocked on system calls that can be\n"
"interrupted by signals, only one of those threads will get\n"
"interrupted, and which one is unpredictable.  If this matters,\n"
"you should probably block all asynchronous signals in all threads\n"
"other than the interpreter's main thread, and not rely on signals\n"
"to interrupt system calls in those threads.\n"
    ),
    .tp_basicsize = sizeof(PeriodicSignalContextObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PSC_new,
    .tp_init = PSC_init,
    .tp_dealloc = PSC_dealloc,
    .tp_methods = PSC_methods,
    .tp_getset = PSC_getsetters,
};

static struct PyModuleDef signaler_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "signaler",
    .m_doc = signaler_doc,
    .m_size = 0,
};

PyMODINIT_FUNC
PyInit_signaler(void)
{
    if (PyType_Ready(&PeriodicSignalContextType) < 0)
        return 0;

    PyObject *mod = PyModule_Create(&signaler_module);
    if (!mod)
        return 0;

    Py_INCREF(&PeriodicSignalContextType);
    if (PyModule_AddObject(mod,
                           "PeriodicSignalContext",
                           (PyObject *)&PeriodicSignalContextType) < 0) {
        Py_DECREF(&PeriodicSignalContextType);
        Py_DECREF(mod);
        return 0;
    }
    return mod;
}
