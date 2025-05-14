# Equivalence proof for the two versions of `timespec_difference_at_least`

This directory contains a machine-checkable proof, in [Lean][], that
the two versions of `timespec_difference_at_least`, implemented by
[`CheckSignalsOftenEnough.c`](../CheckSignalsOftenEnough.c) in the
parent directory, are equivalent.

The proof is probably not written as tidily or as clearly as it could
be; improvements are welcome.  Much gratitude to Yakov Pechersky,
Niels Voss, Aaron Liu, and Kevin Buzzard, from the Lean4 discussion
board, for getting me past several sticking points.

[Lean]: https://lean-lang.org/
