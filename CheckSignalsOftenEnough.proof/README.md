# Equivalence proof for the two versions of `timespec_difference_at_least`

This directory contains an incomplete machine-checkable proof that the
two versions of `timespec_difference_at_least`, implemented by
[`CheckSignalsOftenEnough.c`](../CheckSignalsOftenEnough.c) in the
parent directory, are equivalent.

The remaining gaps (search for the word `sorry` in `Proof.lean`) are
easily bridgeable in the human mind.  I have failed to explain them to
the Lean proof assistant, but perhaps the proof may be interesting
reading anyway.
