# Stochastic test of `timespec_difference_at_least`

Run `cargo test` in this directory to perform a stochastic test of
the equivalence of the two versions of `timespec_difference_at_least`
from [`CheckSignalsOftenEnough.c`](../CheckSignalsOftenEnough.c) in
the parent directory.  Random pairs of `struct timespec` values are
generated, fed to both functions, and the results expected to be equal.

By default only 256 test pairs are generated.  You can do a more
stringent test by setting the environment variable `PROPTEST_CASES`
to a large number.  For example, the author has personally run

```sh
PROPTEST_CASES=$((1024 * 1024 * 1024)) cargo test --release
```

which completed successfully in about 13 minutes on a 2019-era x86.

The test harness should work with any reasonably recent version of Rust.

## Caveat

The test harness does not actually test the code in
`CheckSignalsOftenEnough.c`; it tests a slightly modified copy,
found in [`src/tdal.c`](src/tdal.c) below this directory.
The modifications are only to make both versions of the function
available simultaneously; the code executed is meant to be identical.
If you have a clever idea for how to make the test harness extract
both versions from `CheckSignalsOftenEnough.c`, please file a pull request.
