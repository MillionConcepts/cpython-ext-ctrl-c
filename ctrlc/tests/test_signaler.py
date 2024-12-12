"""A little demo of how to use PeriodicSignalContext."""

from signal import signal, SIGUSR1, SIG_DFL
from threading import Thread, Event
from time import sleep
from ctrlc.signaler import PeriodicSignalContext


CYCLES = 10


def sleep_and_catch(results, cycles, sleep_time):
    """Helper providing the inner loop usually used in these tests."""
    for _ in range(cycles):
        try:
            sleep(sleep_time)
            results.append("finished")
        except KeyboardInterrupt:
            results.append("interrupted")
        except Exception as e:
            results.append(str(e))


def test_always_interrupted():
    """Test PeriodicSignalContext with timing where it should
       interrupt the main thread on every iteration."""
    results = []
    with PeriodicSignalContext(50):
        sleep_and_catch(results, CYCLES, 1)
    assert results == ["interrupted"] * CYCLES


def test_every_other_cycle():
    """Test PeriodicSignalContext with timing where it should
       interrupt the main thread on exactly half of iterations."""
    results = []
    with PeriodicSignalContext(50):
        sleep_and_catch(results, CYCLES, 45/1000)
    assert results == ["finished", "interrupted"] * (CYCLES // 2)


def test_usable_twice():
    """Test that a PeriodicSignalContext object can be entered,
       exited, and entered again, with signals occurring only when
       the context is active."""
    pc = PeriodicSignalContext(50)
    results = []
    with pc:
        sleep_and_catch(results, CYCLES // 2, 1)

    try:
        assert results == ["interrupted"] * (CYCLES // 2)
        sleep(100/1000)
    except KeyboardInterrupt as e:
        raise AssertionError(
            "KeyboardInterrupt occurred with context inactive"
        ) from e

    with pc:
        sleep_and_catch(results, CYCLES // 2, 1)
    assert results == ["interrupted"] * CYCLES


def test_nested_with():
    """Test that a PeriodicSignalContext object can be entered recursively,
       with the signals continuing until the outermost 'with' is exited."""
    pc = PeriodicSignalContext(50)
    results = []
    with pc:
        for _ in range(CYCLES):
            try:
                with pc:
                    sleep(1)
                results.append("finished")
            except KeyboardInterrupt:
                results.append("interrupted")
            except Exception as e:
                results.append(str(e))
    assert results == ["interrupted"] * CYCLES


def test_SIGUSR1():
    """Test delivery of SIGUSR1 instead of SIGINT."""
    results = []
    def handler(sig, _frame):
        results.append(sig)
    try:
        signal(SIGUSR1, handler)
        with PeriodicSignalContext(50, SIGUSR1):
            sleep_and_catch(results, 1, 175/1000)
    finally:
        signal(SIGUSR1, SIG_DFL)

    # The SIGUSR1 handler doesn't raise an exception, it just adds things
    # to 'results'.
    assert results == [SIGUSR1, SIGUSR1, SIGUSR1, "finished"]


def test_signals_handled_by_initial_thread_1():
    """Test that if there is another thread doing something unrelated,
       it doesn't get any KeyboardInterrupts."""
    def bg_threadproc(results, cycles, sleep_time):
        for _ in range(cycles):
            try:
                sleep(sleep_time)
                results.append("tick")
            except BaseException as e:
                results.append(str(e))

    main_results = []
    bg_results = []
    bg_thread = Thread(target=bg_threadproc, args=(bg_results, CYCLES, 50/1000))
    bg_thread.start()
    try:
        with PeriodicSignalContext(50):
            sleep_and_catch(main_results, CYCLES, 1)
    finally:
        bg_thread.join()

    assert main_results == ["interrupted"]*CYCLES
    assert bg_results == ["tick"]*CYCLES


def test_signals_handled_by_initial_thread_2():
    """Test that KeyboardInterrupts are raised on the initial thread
       even if the PeriodicSignalContext was entered in a second thread.
       (This is a design decision of the CPython virtual machine.
       It only ever executes Python-level signal handlers on the
       initial thread, no matter which thread fielded the actual signal.)"""
    def bg_threadproc(results, stop, interval):
        try:
            with PeriodicSignalContext(interval):
                stop.wait()
            results.append("ok")
        except BaseException as e:
            results.append(str(e))

    bg_results = []
    main_results = []
    stop = Event()
    bg_thread = Thread(target=bg_threadproc, args=(bg_results, stop, 50))
    bg_thread.start()
    try:
        sleep_and_catch(main_results, CYCLES, 1)
    finally:
        stop.set()
        bg_thread.join()

    assert main_results == ["interrupted"]*CYCLES
    assert bg_results == ["ok"]
