### Python 3.12 interpreter, C stack trace, waiting for input

```
(gdb) backtrace
#0  0x00007fbfd7528b13 in __GI___select (nfds=1,
    readfds=0x7ffc32d03c00, writefds=0x0, exceptfds=0x0, timeout=0x0)
    at ../sysdeps/unix/sysv/linux/select.c:69
#1  0x00007fbfd7cc4d6b in readline_until_enter_or_signal (
    prompt=<optimized out>, signal=<synthetic pointer>)
    at ./Modules/readline.c:1366
#2  call_readline (sys_stdin=<optimized out>, sys_stdout=<optimized out>,
    prompt=<optimized out>) at ./Modules/readline.c:1419
#3  0x00007fbfd76b4f6e in PyOS_Readline (
    sys_stdin=0x7fbfd75f68e0 <_IO_2_1_stdin_>,
    sys_stdout=0x7fbfd75f75c0 <_IO_2_1_stdout_>, prompt=0x7fbfd6e38838 ">>> ")
    at Parser/myreadline.c:418
#4  0x00007fbfd76b68aa in tok_underflow_interactive (tok=0x558d29643790)
    at Parser/tokenizer.c:1119
...
(gdb) list
69>       int r = SYSCALL_CANCEL (pselect6_time64, nfds, readfds,
70                                writefds, exceptfds, pts64, NULL);
71        if (timeout != NULL)
72          TIMESPEC_TO_TIMEVAL (timeout, pts64);
73        return r;
```

We’re inside the C library.  We’re making a system call.  It doesn’t
say anything about signals.

From outside the debugger I send the interpreter a SIGINT and then I
go back to the debugger and I tell it to let the interpreter execute
_one machine instruction_.

```
(gdb) stepi
signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
347	{
```

We’re back inside CPython!  There’s no call to `signal_handler` in the
C library code!  What the heck?!

```
(gdb) backtrace
#0  signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
#1  <signal handler called>
#2  0x00007fbfd7528b13 in __GI___select (nfds=1, readfds=0x7ffc32d03c00,
    writefds=0x0, exceptfds=0x0, timeout=0x0)
    at ../sysdeps/unix/sysv/linux/select.c:69
#3  0x00007fbfd7cc4d6b in readline_until_enter_or_signal (prompt=<optimized out>,
    signal=<synthetic pointer>) at ./Modules/readline.c:1366
```

This is the black magic of signal handlers: They get called when you
never called them yourself.

CPython wasn’t _running_ when the signal happened.  It was waiting for
input.  The OS had saved its registers and its “program counter” and
given the CPU to some other program.

Riddle me this: Does anything prevent the operating system from
resuming execution of CPython _somewhere else_ than where it left off?

Like maybe, at the first instruction of `signal_handler`?

Having first forged a stack frame, to make the CPU _behave as if_
there was a `call` from where it actually did leave off, to
`signal_handler`, so that when `signal_handler` returns, things
will continue as expected?

```
(gdb) finish
Run till exit from #0  signal_handler (sig_num=2) at ./Modules/signalmodule.c:347
<signal handler called>
(gdb) finish
Run till exit from #0  <signal handler called>
0x00007fbfd7528b13 in __GI___select (nfds=1, readfds=readfds@entry=0x7ffc32d03c00,
    writefds=writefds@entry=0x0, exceptfds=exceptfds@entry=0x0, timeout=timeout@entry=0x0)
    at ../sysdeps/unix/sysv/linux/select.c:69
69	  int r = SYSCALL_CANCEL (pselect6_time64, nfds, readfds,
writefds, exceptfds,
(gdb) finish
Run till exit from #0  0x00007fbfd7528b13 in __GI___select (nfds=1,
    readfds=readfds@entry=0x7ffc32d03c00, writefds=writefds@entry=0x0,
    exceptfds=exceptfds@entry=0x0, timeout=timeout@entry=0x0)
    at ../sysdeps/unix/sysv/linux/select.c:69
0x00007fbfd7cc4d6b in readline_until_enter_or_signal (prompt=<optimized out>,
    signal=<synthetic pointer>) at ./Modules/readline.c:1366
1366	            has_input = select(fileno(rl_instream) + 1, &selectset,
Value returned is $1 = -1
```

Huh, `select` returned even though it was told to wait forever for input.

This is the other special trick of signal handlers: They make system
calls return early.

The last thing you need to know at this point is that a signal handler
can be called _at any time_.  The OS doesn’t have to wait for the
program to make a system call.  It can _preempt_ execution of a
running program, _at any time_, forge that special stack frame, and
make the program run its signal handler before it executes any more
normal instructions.

### Signal Safety
