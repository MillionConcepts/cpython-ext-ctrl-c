static const char signaler_doc[] =
    "C extension which generates signals from a thread"
    " independent of the CPython GIL.";

// Copyright 2024 Million Concepts LLC
// BSD-3-Clause License
// See LICENSE.md for details

#define _XOPEN_SOURCE 700
#define PY_SSIZE_T_CLEAN 1
#include <Python.h>

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

// 1e9 nanoseconds in a second
#define NS_PER_S (1000 * 1000 * 1000)

// One-shot event implemented on top of a pthread condition variable.
// Yes, this is hideous.  Pthread condition variables are a bad design.
// An earlier version of this code used a pipe instead, and that was
// much shorter and easier to understand, but it needed ppoll(), which
// is not in POSIX.
typedef struct OneShotEvent {
    pthread_cond_t cv;
    pthread_mutex_t mutex;
    bool signaled;
} OneShotEvent;

// Initialize a OneShotEvent.  Returns 0 on success or -1 with a
// Python exception set on failure.
static int OSE_init(OneShotEvent *ev)
{
    int err;

    err = pthread_cond_init(&ev->cv, 0);
    if (err) {
        errno = err;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    err = pthread_mutex_init(&ev->mutex, 0);
    if (err) {
        pthread_cond_destroy(&ev->cv);
        errno = err;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    ev->signaled = false;
    return 0;
}

// Destroy a OneShotEvent.
static void OSE_destroy(OneShotEvent *ev)
{
    pthread_cond_destroy(&ev->cv);
    pthread_mutex_destroy(&ev->mutex);
}

// Signal a OneShotEvent.  Call *without* the mutex held.
static void OSE_signal(OneShotEvent *ev)
{
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = true;
    pthread_cond_broadcast(&ev->cv);
    pthread_mutex_unlock(&ev->mutex);
}

// Reset a OneShotEvent.  Call *without* the mutex held.
static void OSE_reset(OneShotEvent *ev)
{
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = false;
    pthread_mutex_unlock(&ev->mutex);
}

// Wait for a OneShotEvent to be signaled.  Call *with* the mutex held.
// If DELAY is not NULL, it represents an amount of time to wait before
// giving up (*not* a clock time to wait until).  Returns true if the
// event was signaled, false if the timeout expired.
static bool OSE_wait(OneShotEvent *ev, const struct timespec *delay)
{
    if (delay) {
        // pthread_cond_timedwait expects the absolute clock time to
        // wait until, not the amount of time to wait.  Yay, struct
        // timespec arithmetic.
        struct timespec stop_waiting_at;
        clock_gettime(CLOCK_REALTIME, &stop_waiting_at);
        stop_waiting_at.tv_sec += delay->tv_sec;
        stop_waiting_at.tv_nsec += delay->tv_nsec;
        while (stop_waiting_at.tv_nsec >= NS_PER_S) {
            stop_waiting_at.tv_sec += 1;
            stop_waiting_at.tv_nsec -= NS_PER_S;
        }

        // The *reason* pthread_cond_timedwait takes an absolute
        // clock time, is because "spurious wakeups may occur",
        // i.e. pthread_cond_timedwait is allowed to unblock and
        // return for no reason at all.  Did I mention this API
        // is badly designed?  Anyway, taking an absolute time does
        // mean we don't have to recalculate the remaining delay on
        // each iteration of this loop.
        while (!ev->signaled) {
            int err = pthread_cond_timedwait(&ev->cv, &ev->mutex,
                                             &stop_waiting_at);
            if (err == ETIMEDOUT)
                return false;
            if (err != 0 && err != EINTR)
                // various other error codes all indicate some form
                // of catastrophic, unrecoverable abuse of the mutex
                abort();
        }
    } else {
        // Spurious wakeups can happen for pthread_cond_wait too.
        while (!ev->signaled) {
            pthread_cond_wait(&ev->cv, &ev->mutex);
        }
    }
    return true;
}

typedef struct PeriodicSignalContextObject {
    PyObject_HEAD

    // Interval at which to send signals
    struct timespec interval;

    // pthread handle to the signal thread
    pthread_t signal_thread;

    // Process ID of this whole process.
    pid_t pid;

    // The number of the signal to be sent.  Zero if this object's
    // __init__ has not yet been called.
    int signal;

    // Depth of calls to __enter__.
    unsigned int entry_count;

    // When this event is signaled the signal thread should exit.
    OneShotEvent stop;
} PeriodicSignalContextObject;

static void *
periodic_signal_threadproc(void *s)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;

    // CAUTION: This is the thread procedure of a bare thread; it has
    // no affiliation with the Python interpreter.  It is not safe to
    // call any CPython API function from here, nor to access any of
    // the Python object head fields of `self`.

    // Block all asynchronous signals in this thread.
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

    // As the thread that receives the stop signal, we need to hold
    // the stop signal's internal mutex at all times except when
    // blocked inside OSE_wait.
    pthread_mutex_lock(&self->stop.mutex);

    for (;;) {
        if (OSE_wait(&self->stop, &self->interval))
            break;

        // Interval expired, send the signal.  If this fails, bail out.
        if (kill(self->pid, self->signal))
            break;
    }

    pthread_mutex_unlock(&self->stop.mutex);
    return 0;
}

// Stop and join the signal thread. Called by PSC_exit and PSC_dealloc.
static int
stop_signal_thread(PeriodicSignalContextObject *self)
{
    OSE_signal(&self->stop);
    pthread_join(self->signal_thread, 0);
    OSE_reset(&self->stop);

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

    if (OSE_init(&self->stop))
        return 0;

    self->pid = getpid();

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
    return 0;
}

static void
PSC_dealloc(PyObject *s)
{
    PeriodicSignalContextObject *self = (PeriodicSignalContextObject *)s;
    if (self->entry_count > 0) {
        stop_signal_thread(self);
        self->entry_count = 0;
    }
    OSE_destroy(&self->stop);
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
