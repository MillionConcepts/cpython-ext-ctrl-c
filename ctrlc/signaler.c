static const char signaler_doc[] =
    "C extension which generates signals from a thread"
    " independent of the CPython GIL.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _GNU_SOURCE 1 // for ppoll()
#define PY_SSIZE_T_CLEAN 1
#include <Python.h>

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

typedef struct PeriodicSignalContextObject {
    PyObject_HEAD

    // Interval at which to send signals
    struct timespec interval;

    // pthread handle to the main interpreter thread
    pthread_t main_thread;

    // pthread handle to the signal thread
    pthread_t signal_thread;

    // Pipe used as a one-bit event queue to tell the signal thread
    // when it needs to stop.
    int stop_event[2];

    // The number of the signal to be sent.  Zero if this object's
    // __init__ has not yet been called.
    int signal;

    // Depth of calls to __enter__.
    unsigned int entry_count;

} PeriodicSignalContextObject;

static void *
periodic_signal_threadproc(void *s)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;

    // CAUTION: This is the thread procedure of a bare thread; it has
    // no affiliation with the Python interpreter.  It is not safe to
    // call any CPython API function from here, nor to access any of
    // the Python object head fields of `self`.

    // Block all signals in this thread, except for those that reflect
    // synchrous CPU exceptions.
    sigset_t ss;
    sigfillset(&ss);
    sigdelset(&ss, SIGABRT);
    sigdelset(&ss, SIGBUS);
    sigdelset(&ss, SIGFPE);
    sigdelset(&ss, SIGILL);
    sigdelset(&ss, SIGSEGV);
    sigdelset(&ss, SIGTRAP);
#ifdef SIGEMT
    sigdelset(&ss, SIGEMT);
#endif
#ifdef SIGIOT
    sigdelset(&ss, SIGIOT);
#endif
    pthread_sigmask(SIG_BLOCK, &ss, 0);

    for (;;) {
        struct pollfd poll[1];
        poll[0].fd = self->stop_event[0];
        poll[0].events = POLLIN;
        poll[0].revents = 0;

        // Use ppoll() to wait for the interval to expire.
        // If it returns early, either because of an error or because
        // something was written to the pipe, that's our cue to exit.
        // Unlike select, ppoll guarantees not to overwrite its
        // timeout argument.
        if (ppoll(poll, 1, &self->interval, 0) != 0) {
            break;
        }

        // Interval expired, send the signal.
        // If this fails, bail out.
        if (pthread_kill(self->main_thread, self->signal)) {
            break;
        }
    }

    // Drain the pipe before returning.  Only one byte should've been
    // written, but we allow for more.
    char sink[256];
    do {} while (read(self->stop_event[0], sink, sizeof sink) > 0);
    return 0;
}

// Stop and join the signal thread. Called by PSC_exit and PSC_dealloc.
static int
stop_signal_thread(PeriodicSignalContextObject *self)
{
    // Stop and join the signal thread.
    char ping = 0;
    if (write(self->stop_event[1], &ping, sizeof ping) != 1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    int err = pthread_join(self->signal_thread, 0);
    if (err) {
        errno = err;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    // I'd like to set ->signal_thread to the equivalent of NULL
    // here, but there is no such equivalent.

    return 0;
}

// We override tp_new for two reasons: to make absolutely sure that
// the kernel objects held in a PeriodicSignalContextObject are
// allocated once and only once, and to ensure that ->signal
// is zeroed when PSC_init is called for the first time.
static PyObject *
PSC_new(PyTypeObject *type, PyObject *Py_UNUSED(a), PyObject *Py_UNUSED(k))
{
    PeriodicSignalContextObject *self =
        (PeriodicSignalContextObject *) type->tp_alloc(type, 0);
    if (!self)
        return 0;

    if (pipe2(self->stop_event, O_CLOEXEC | O_NONBLOCK)) {
        Py_DECREF(self);
        return NULL;
    }

    // ??? We want this to be the thread in which PyErr_CheckSignals
    // does something, but I don't think there's actually any
    // guarantee that it is.  I can't find anything in the C-API
    // manual that would allow me to look up the pthread_t handle of
    // the thread that called Py_Initialize.  This is why there's
    // a dire warning in the PeriodicSignalContext docstring about
    // using it only from the initial thread.
    self->main_thread = pthread_self();

    // I'd like to set ->signal_thread to the equivalent of NULL
    // here, but there is no such equivalent.

    self->interval.tv_sec = 0;
    self->interval.tv_nsec = 0;
    self->signal = 0;
    self->entry_count = 0;
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

    double interval_ms = 0;
    int signal = SIGINT;
    static char *kwlist[] = {
        "interval", "signal", 0
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                                     "d|i:PeriodicSignalContext", kwlist,
                                     &interval_ms, &signal))
        return -1;

    // This sigaction call will fail if `signal` is not a valid signal
    // number; its only other effect is to write data to `dummy`.
    struct sigaction dummy;
    if (sigaction(signal, 0, &dummy) != 0) {
        PyErr_Format(PyExc_ValueError, "%d is not a valid signal number",
                     signal);
        return -1;
    }

    if (!isfinite(interval_ms) || interval_ms <= 0) {
        PyErr_SetString(PyExc_ValueError,
                        "interval must be positive and finite");
        return -1;
    }
    // The smallest time interval representable by struct timespec is
    // 1 ns = 1e-6 ms.
    if (interval_ms < 1e-6) {
        PyErr_SetString(PyExc_ValueError, "minimum interval is 1 ns (1e-6 ms)");
        return -1;
    }

    time_t interval_sec = (time_t)floor(interval_ms / 1000.0);
    unsigned long interval_ns =
        (unsigned long)floor(fmod(interval_ms, 1000) * 1e6);

    self->signal = signal;
    self->interval.tv_sec = interval_sec;
    self->interval.tv_nsec = interval_ns;
    return 0;
}

static void
PSC_dealloc(PyObject *s)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    if (self->entry_count > 0) {
        stop_signal_thread(self);
    }
    close(self->stop_event[0]);
    close(self->stop_event[1]);
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
        int err = pthread_create(&self->signal_thread, 0,
                                 periodic_signal_threadproc,
                                 (void *)self);
        if (err) {
            errno = err;
            return PyErr_SetFromErrno(PyExc_OSError);
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
        if (stop_signal_thread(self))
            return NULL;
    }
    if (self->entry_count > 0)
        self->entry_count -= 1;
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
    double ms = self->interval.tv_sec * 1000.0
              + self->interval.tv_nsec * 1.0e-6;
    return PyFloat_FromDouble(ms);
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
        "with PeriodicSignalContext(100, signal=signal.SIGINT):\n"
        "   do_stuff_that_gets_interrupted_repeatedly()\n"
        "\n"
        "Within the context established by using a PeriodicSignalContext\n"
        "object in a `with` statement, Unix signals will be sent to the\n"
        "main interpreter thread periodically.  The first (mandatory)\n"
        "argument is the interval at which to send signals, in milliseconds.\n"
        "The second (optional) argument is the signal to send, defaulting to\n"
        "signal.SIGINT.\n"
        "\n"
        "PeriodicSignalContext can only be used from the initial thread.\n"
        "If you try to use it from any other thread, it may malfunction\n"
        "unpredictably.\n"
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
