# Writing Extension Modules To Be Interruptible

Category: ???
Duration: 30 minutes
Level: Advanced

## Audience

This talk is for experienced programmers.  It will be primarily of
interest to authors of extension modules that do heavy number
crunching (e.g. NumPy, PyTorch), data compression and encoding (zstd,
libnacl), or any other processing that runs for a long time before
returning to the Python interpreter.

Authors of tools and frameworks intended to make it easier to write
this kind of extension module (e.g. Cython, Numba, PyO3) will also be
interested, and possibly also core CPython developers.

## Objectives

Attendees will learn how to write extension modules that run for a
long time but still return to the Python interpreter *promptly* when
interrupted by the user pressing control-C or similar.  We will also
explore why this is not as easy as it might seem and how it could be
made easier.

## Description

Have you ever told Python to crunch some numbers, realized right after
pressing RETURN that you made a mistake setting up the problem, hit
control-C to stop it again...and then sat there twiddling your thumbs
for a surprisingly long time as nothing seemed to happen?

The short version of what's going on when this happens is, the
Python interpreter is notified of your having hit control-C almost
immediately, but it has to wait to throw `KeyboardInterrupt` until
the extension module that's doing the number crunching finishes
its calculation and returns control to the interpreter.

In this talk we will discuss why the interpreter works that way (and
why it *has* to work that way), what extension module authors can do
today to make their extensions notice control-C promptly, and how we
can all work toward a future where it's easy and natural for extension
module authors to write extensions that don't make you wait for them
to stop.

## Detailed Abstract

When you type control-C at a running Python program, the Python
interpreter raises a `KeyboardInterrupt` exception, which stops the
program in its tracks and returns to the Python "read-eval-print" loop
or the shell or whatever outer context there is.  But, if the program
is currently executing the code of an extension module, rather than
code actually *written in* Python, there is often a delay before the
`KeyboardInterrupt` actually happens, and there's no upper limit on how
long that delay can be.

The Python *interpreter* is notified of the control-C almost
immediately (unless something is wrong at a much deeper level), so why
does this delay happen?  In fact, it's an intentional design decision:
the interpreter doesn't know if it's *safe* to throw an exception
while an extension module is running.  So when it gets the
notification, it just sets an internal flag, "`KeyboardInterrupt`
should happen soon," and lets the extension module carry on with its
work.  It will actually raise KeyboardInterrupt when the extension
returns to the main interpreter loop.  Whenever that happens.  *If* it
ever happens.

The extension module has the *option* of checking that internal flag
itself and, if it's set, wrapping up its work early, but nobody can
force extension authors to take that option.  In fact, just to *know*
about that option, you have to dig deep into the C-API reference
manual and then realize the implications of what it's telling you.
Once you do know, you then have to think about when and how often you
ought to be checking the flag, and, in the worst case, restructure
your whole library to make it *possible* to stop the calculation
early.  This task is difficult enough that even the most popular
and well-maintained extensions frequently do not bother.  Tools like
Cython don't help at all, although they could.

I will walk attendees through the changes that need to be made to a
relatively simple extension module in order for it to respond quickly
to control-C, discuss the costs and tradeoffs involved, talk about
what options we have when the changes that would be required are
infeasible, and finally outline a few possibilities for changes to
core CPython and to tools like Cython and PyO3 that would make the
costs lower and the hard cases easier.

## Outline

1. Introduce myself (1 min)

2. Introduce the problem (4 min)
    - there can be a delay in responding to Control-C (1 min)
    - this happens because CPython can't interrupt execution of an
      extension module without the extension's cooperation
      (1 min)
    - uncooperative interrupts aren't just "not yet implemented,"
      they're flat-out impossible because only the extension
      can deallocate any resources it allocated (1 min)
    - extensions *can* cooperate but many extension authors find it
      difficult or don't even know they ought to (1 min)

3. Illustrate what extension authors have to do today (16 min)
    - insert calls to `PyErr_CheckSignals` at appropriate points;
      if it tells you to, clean up and exit (3 min)
    - these calls have substantial overhead, especially if you dropped
      the GIL, so you want to do them as little as possible (2 min)
    - you only *need* to do them once every few milliseconds for
      prompt responsiveness to control-C (1 min)
    - your algorithm might or might not offer an easy way to limit
      calls to `PyErr_CheckSignals` to an appropriate rate (3 min)
      a. simple loops over arrays: do it every N iterations, easy
         (except for picking N) (1 min)
      b. recursion is troublesome (2 min)
    - can cheat by monitoring elapsed time since the last check (2 min)
    - threads introduce more problems (3 min)
      - only one thread actually takes SIGINT (1 min)
      - `PyErr_CheckSignals` is a no-op off the main thread (1 min)
      - together these mean your "clean up and exit" actions might get
        stuck (1 min)

4. Ways to make this easier and more efficient
   - Ideas I have (6 min)
      a. Improve core documentation (1 min)
      b. `PyErr_CheckSignals` could be much cheaper (1 min)
      c. Tools like Cython and Numba could insert checks and cleanup
         code for you (2 min)
      d. PyO3 could make creative use of Rust's `async` support to
         insert checks and cleanup code for you (2 min)

   - Invite audience to call out additional one-sentence ideas,
     I'll respond briefly to each
     (remaining time, segue into general Q&A)
