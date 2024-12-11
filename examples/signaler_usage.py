"""A little demo of how to use PeriodicSignalContext."""

from sys import stderr
from time import sleep
from ctrlc.signaler import PeriodicSignalContext

def main():
    pc = PeriodicSignalContext(50)
    with pc:
        # In this loop, we expect the sleep to be interrupted
        # every single time.
        for _ in range(5):
            try:
                sleep(1)
                stderr.write(".")
            except KeyboardInterrupt:
                stderr.write("!")
            except Exception as e:
                stderr.write("E")
            stderr.flush()

    stderr.write('\n')
    stderr.flush()
    sleep(1)

    with pc:
        # In this loop, we expect the sleep to be interrupted
        # roughly 50% of the time.
        for _ in range(10):
            try:
                sleep(49/1000)
                stderr.write(".")
            except KeyboardInterrupt:
                stderr.write("!")
            except Exception as e:
                stderr.write("E")
            stderr.flush()
    stderr.write('\n')
    stderr.flush()

main()
