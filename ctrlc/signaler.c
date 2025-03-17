static const char signaler_doc[] =
    "C extension which generates a signal upon expiration of a timer,"
    " either once or repeatedly.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _XOPEN_SOURCE 700
#define PY_SSIZE_T_CLEAN 1
#include <Python.h>
#include <stdbool.h>
#include <signal.h>

// 1e9 nanoseconds in a second
#define NS_PER_S (1000 * 1000 * 1000)

typedef struct TimerObject {
    PyObject_HEAD

    // Timer object.  If __init__ has not yet been called,
    // the value of this field is indeterminate.
    timer_t timer;

    // Interval at which to send signals
    struct timespec interval;

    // Depth of calls to __enter__.
    unsigned int entry_count;

    // The number of the signal to be sent.  Zero if this object's
    // __init__ has not yet been called.
    unsigned short signal;

    // True to send signals repeatedly until the context is exited.
    // False to send just one signal.
    bool repeat : 1;

    // True if this object's __init__ method has been called and
    // therefore ->timer is valid.
    bool ready : 1;

} TimerObject;


// We override tp_new just to make absolutely sure that ->ready is
// zeroed when Timer_init is called for the first time.
static PyObject *
Timer_new(PyTypeObject *type, PyObject *Py_UNUSED(a), PyObject *Py_UNUSED(k))
{
    TimerObject *self =
        (TimerObject *) type->tp_alloc(type, 0);
    if (!self)
        return 0;

    // there is no portable way to initialize a timer_t field other
    // than by calling timer_create, and we can't call timer_create
    // until __init__

    self->interval.tv_sec = 0;
    self->interval.tv_nsec = 0;
    self->entry_count = 0;
    self->signal = 0;
    self->repeat = false;
    self->ready = false;
    return (PyObject *)self;
}

static int
Timer_init(PyObject *s, PyObject *args, PyObject *kwds)
{
    TimerObject *self = (TimerObject *)s;
    if (self->ready) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Timer.__init__ called twice");
        return -1;
    }

    double interval = 0;
    int signal = SIGINT;
    int repeat = 1;
    static char *kwlist[] = {
        "interval", "signal", "repeat", 0
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                                     "d|ip:Timer", kwlist,
                                     &interval, &signal, &repeat))
        return -1;

    // We presume that valid signal numbers fit in the range of
    // 'unsigned short' and that this is smaller than the range of 'int'.
    if (signal < 0 || signal > (int)USHRT_MAX) {
        PyErr_Format(PyExc_ValueError, "%d is not a valid signal number",
                     signal);
    }

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

    double interval_sec = floor(interval);
    double interval_ns = floor((interval - interval_sec) * 1e9);

    self->interval.tv_sec = (time_t) interval_sec;
    self->interval.tv_nsec = (long) interval_ns;
    self->signal = (unsigned short) signal;
    self->repeat = repeat;
    self->ready = true;

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
Timer_dealloc(PyObject *s)
{
    TimerObject *self = (TimerObject *)s;
    timer_delete(self->timer);
}

static PyObject *
Timer_enter(PyObject *s, PyObject *Py_UNUSED(ignored))
{
    TimerObject *self = (TimerObject *)s;
    if (self->entry_count == UINT_MAX) {
        PyErr_SetString(PyExc_RuntimeError,
            "too many nested calls to Timer.__enter__");
        return NULL;
    }
    if (self->entry_count == 0) {
        struct itimerspec arm;
        arm.it_value.tv_sec = self->interval.tv_sec;
        arm.it_value.tv_nsec = self->interval.tv_nsec;

        if (self->repeat) {
            arm.it_interval.tv_sec = self->interval.tv_sec;
            arm.it_interval.tv_nsec = self->interval.tv_nsec;
        } else {
            arm.it_interval.tv_sec = 0;
            arm.it_interval.tv_nsec = 0;
        }

        if (timer_settime(self->timer, 0, &arm, 0)) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
    }
    self->entry_count += 1;
    return Py_NewRef(self);
}

static PyObject *
Timer_exit(PyObject *s, PyObject *Py_UNUSED(ignored))
{
    TimerObject *self = (TimerObject *)s;
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
Timer_get_signal(PyObject *s, void *Py_UNUSED(ignored))
{
    TimerObject *self = (TimerObject *)s;
    return PyLong_FromLong(self->signal);
}

static PyObject *
Timer_get_interval(PyObject *s, void *Py_UNUSED(ignored))
{
    TimerObject *self = (TimerObject *)s;
    return PyFloat_FromDouble(
        (double)self->interval.tv_sec
      + (double)self->interval.tv_nsec * 1.0e-9
    );
}

static PyObject *
Timer_get_repeat(PyObject *s, void *Py_UNUSED(ignored))
{
    TimerObject *self = (TimerObject *)s;
    return PyBool_FromLong(self->repeat);
}

static PyMethodDef Timer_methods[] = {
    { "__enter__", Timer_enter, METH_NOARGS,
      "Start sending signals.  See class docs for details." },
    { "__exit__", Timer_exit, METH_VARARGS,
      "Stop sending signals.  See class docs for details." },
    { 0, 0, 0, 0 },
};

static PyGetSetDef Timer_getsetters[] = {
    { "signal", Timer_get_signal, 0,
      "The signal being sent", 0 },
    { "interval", Timer_get_interval, 0,
      "Interval between signals, in milliseconds", 0 },
    { "repeat", Timer_get_repeat, 0,
      "True if the signal repeats, false if it is sent only once", 0 },
    { 0, 0, 0, 0, 0 }
};

static PyTypeObject TimerType = {
    PyVarObject_HEAD_INIT(0, 0)
    .tp_name = "signaler.Timer",
    .tp_doc = PyDoc_STR(
"with Timer(0.1, signal=signal.SIGINT, repeat=True):\n"
"   do_stuff_that_gets_interrupted()\n"
"\n"
"Within the context established by using a Timer object\n"
"in a `with` statement, Unix signals will be sent to the interpreter\n"
"process, either periodically or just once.\n"
"\n"
"The first (mandatory) argument is the interval at which to\n"
"send signals; like time.sleep, this is in seconds, and may be a\n"
"floating-point number to express an interval shorter than one second.\n"
"\n"
"The 'signal' argument is the number of the signal to send,\n"
"defaulting to signal.SIGINT.  With the default treatment of\n"
"this signal, that means your program will field periodic\n"
"KeyboardInterrupt exceptions.\n"
"\n"
"If the 'repeat' argument is True, the signal will repeat;\n"
"if it is false, the signal will be sent only once each time the\n"
"Timer context is entered.\n"
"\n"
"Note that Python-level signal handlers are only ever executed on\n"
"the interpreter's \"main thread\" (usually the initial thread of\n"
"the interpreter process).  If you create a Timer\n"
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
    .tp_basicsize = sizeof(TimerObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Timer_new,
    .tp_init = Timer_init,
    .tp_dealloc = Timer_dealloc,
    .tp_methods = Timer_methods,
    .tp_getset = Timer_getsetters,
};

static struct PyModuleDef signaler_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "signaler",
    .m_doc = signaler_doc,
    .m_size = 0,
};

// called via dlsym; pacify -Wmissing-prototypes
extern PyMODINIT_FUNC PyInit_signaler(void);

PyMODINIT_FUNC
PyInit_signaler(void)
{
    if (PyType_Ready(&TimerType) < 0)
        return 0;

    PyObject *mod = PyModule_Create(&signaler_module);
    if (!mod)
        return 0;

    Py_INCREF(&TimerType);
    if (PyModule_AddObject(mod,
                           "Timer",
                           (PyObject *)&TimerType) < 0) {
        Py_DECREF(&TimerType);
        Py_DECREF(mod);
        return 0;
    }
    return mod;
}
