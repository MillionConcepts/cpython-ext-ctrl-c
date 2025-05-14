/- This file contains a machine-checkable proof that the optimized
- condition found in CheckSignalsOftenEnough.c, for 'more than 1
- millisecond has elapsed since the last check', is equivalent to
- a more straightforward calculation.
-/

import Mathlib
import Mathlib.Tactic

-- Conversion factor from seconds to nanoseconds (10⁹)
abbrev ONE_S_IN_NS : Nat := 1000000000

-- Lean analogue of `struct timespec`.  A Timespec represents an
-- interval of time since some unspecified epoch instant.  We seek to
-- prove an equivalence between two methods of computing whether the
-- *difference* between two Timespec quantities is greater than some
-- interval, so we don't care what the epoch actually is.
--
-- In C, the seconds field is a `time_t`, which is normally either a
-- 32- or 64-bit signed machine integer.  For our purposes—measuring
-- time intervals typically no more than a few tens of seconds—we can
-- ignore the possibility of machine integer overflow and model the
-- seconds field using Int, i.e. a mathematical integer with unbounded
-- range.
--
-- In C, the nanoseconds field is a `long` (yes, signed, for no good
-- reason) and is constrained to the range [0, ONE_S_IN_NS) by
-- convention alone.  Fin has properties we don't want
-- (e.g. arithmetic on Fin is defined to be modulo its limit) and is
-- also just awkward to work with.  Instead, our version of Timespec
-- uses Nat for the nanoseconds field and, rather like Fin, bundles a
-- proof that it's in the appropriate range.
structure Timespec where
  sec        : Int
  nsec       : Nat
  nsec_bound : (nsec < ONE_S_IN_NS)
deriving Repr

-- This function defines the conversion from a Timespec quantity to
-- the total number of nanoseconds that have elapsed since that
-- Timespec's (unspecified) epoch.
def timespec_to_ns (tv: Timespec) : Int :=
  tv.sec * ONE_S_IN_NS + tv.nsec

-- This is the SIMPLE_TIMESPEC_DIFFERENCE version of
-- timespec_difference_at_least from CheckSignalsOftenEnough.c.
--
-- The calculation it performs is valid for any `min_ns: Nat`, but we limit
-- it to `min_ns: Fin ONE_S_IN_NS` so that an `=` equivalence between this
-- function and timespec_difference_at_least_cases, below, is type correct.
def timespec_difference_at_least_mul
  (after : Timespec) (before: Timespec) (min_ns: Fin ONE_S_IN_NS) : Bool :=
  let delta := (timespec_to_ns after) - (timespec_to_ns before);
  delta < 0 ∨ delta ≥ min_ns.val

-- This is the cleverer version of timespec_difference_at_least that avoids
-- needing to do any multiplication.
--
-- Unlike timespec_difference_at_least_mul, it *does* rely on `min_ns`
-- being in the specified finite range.
def timespec_difference_at_least_cases
  (after : Timespec) (before: Timespec) (min_ns: Fin ONE_S_IN_NS) : Bool :=
  if after.sec = before.sec then
    after.nsec < before.nsec ∨ after.nsec ≥ before.nsec + min_ns.val
  else if after.sec = before.sec + 1 then
    (ONE_S_IN_NS + after.nsec) - before.nsec ≥ min_ns.val
  else
    -- Either after.sec < before.sec, or after.sec > before_sec + 1.
    -- In the former case, (timespec_to_ns after) < (timespec_to_ns before)
    -- no matter what the nsec values are.  In the latter case,
    -- (timespec_to_ns after) - (timespec_to_ns before) must be at
    -- least one whole second and therefore greater than any possible
    -- value of min_ns.
    true

-- There are four cases to consider for the proof, depending on the
-- relationship between after.sec and before.nsec:
--   after.sec < before.sec
--   after.sec = before.sec
--   after.sec = before.sec + 1
--   after.sec > before.sec + 1
-- These are exhaustive, by trichotomy of the ordering relation on ℕ.
--
-- We prove each case individually.

-- The after.sec = before.sec case, which is the easiest.
-- We don't need any of the bounds on an, bn, or m.
lemma case_as_eq_bs (an bn m: ℕ)
  : an < bn ∨ (m:ℤ) ≤ (an:ℤ) - (bn:ℤ) ↔ an < bn ∨ bn + m ≤ an
  := by
  apply or_congr_right'
  intro h
  zify
  exact le_sub_iff_add_le'

-- Simplification lemma used by the remaining cases:
lemma x_ne_x_plus_stuff (x: ℤ) (y: ℕ)
  : ¬(x = x + 1 + (y:ℤ) + 1)
  := by linarith

-- The after.sec < before.sec case, which is the second easiest since
-- both functions always return true.
lemma case_as_lt_bs
  (as : ℤ)
  (δs an bn m : ℕ)
  (an_bound : an < ONE_S_IN_NS)
  : as * 1000000000 + ↑an < (as + 1 + ↑δs) * 1000000000 + ↑bn
    ∨ ↑m ≤ as * 1000000000 + ↑an - ((as + 1 + ↑δs) * 1000000000 + ↑bn)
    ↔ ¬as = as + 1 + ↑δs + 1 ∨ m ≤ 1000000000 + an - bn
  := by
    -- the right-hand side of the equivalence is always true
    simp [x_ne_x_plus_stuff]
    -- now we have the disjunction
    -- ⊢ as * 1000000000 + ↑an < (as + 1 + ↑δs) * 1000000000 + ↑bn
    --   ∨ ↑m ≤ as * 1000000000 + ↑an - ((as + 1 + ↑δs) * 1000000000 + ↑bn)
    -- the left side of which is always true
    left
    -- cancel duplicate terms on both sides of the remaining inequality
    rewrite [add_mul, add_mul, add_assoc, add_assoc, Int.add_lt_add_iff_left]
    norm_cast
    -- now we have an < 1*1000000000 + (δs * 1000000000 + bn),
    -- with all variables Nat,
    -- and we know an < 1000000000
    revert an_bound
    simp [ONE_S_IN_NS]
    exact Nat.lt_add_right (δs * 1000000000 + bn)

-- The after.sec > before.sec + 1 case, which is very similar to the
-- after.sec < before.sec case.
lemma case_as_gt_bs_plus_one
  (bs : ℤ)
  (an bn m q : ℕ)
  (m_bound : m < ONE_S_IN_NS)
  (bn_bound : bn < ONE_S_IN_NS)
  : (bs + 1 + ↑(q + 1)) * 1000000000 + ↑an < bs * 1000000000 + ↑bn
    ∨ ↑m ≤ (bs + 1 + ↑(q + 1)) * 1000000000 + ↑an - (bs * 1000000000 + ↑bn)
    ↔ ¬q + 1 = 0 ∨ m ≤ 1000000000 + an - bn
  := by
    -- the right-hand side of the equivalence is always true
    simp [Nat.add_one_ne_zero]
    -- now we have the disjunction
    -- ⊢ (bs + 1 + (↑q + 1)) * 1000000000 + ↑an < bs * 1000000000 + ↑bn
    --   ∨ ↑m ≤ (bs + 1 + (↑q + 1)) * 1000000000 + ↑an
    --           - (bs * 1000000000 + ↑bn)
    -- the right side of which is always true, but proving it requires
    -- both upper bounds and a bunch of algebraic rearrangement
    right
    ring_nf
    -- ⊢ ↑m ≤ 2000000000 + ↑q * 1000000000 + (↑an - ↑bn)
    zify at m_bound bn_bound
    refine m_bound.le.trans ?_
    rewrite [add_sub, le_sub_iff_add_le]
    -- ⊢ 1000000000 + ↑bn ≤ 2000000000 + ↑q * 1000000000 + ↑an
    refine (add_le_add_left bn_bound.le _).trans ?_
    norm_num
    -- ⊢ 2000000000 ≤ 2000000000 + ↑q * 1000000000 + ↑an
    rewrite [add_assoc, le_add_iff_nonneg_right]
    positivity

-- The most interesting case is when after.sec = before.sec + 1.
lemma case_as_eq_bs_plus_one
  (bs : ℤ)
  (an bn m : ℕ)
  (bn_bound : bn < ONE_S_IN_NS)
  : (bs + 1 + ↑0) * 1000000000 + ↑an < bs * 1000000000 + ↑bn
    ∨ ↑m ≤ (bs + 1 + ↑0) * 1000000000 + ↑an - (bs * 1000000000 + ↑bn)
    ↔ ¬0 = 0 ∨ m ≤ 1000000000 + an - bn
  := by
    -- turn `m ≤ 1000000000 + an - bn` into `bn + m ≤ 1000000000 + an`
    rewrite [le_tsub_iff_left (b := m) (bn_bound.le.trans (by simp))]
    -- normalize
    zify
    ring_nf
    -- ⊢     1000000000 + bs * 1000000000 + ↑an < bs * 1000000000 + ↑bn
    --     ∨ ↑m ≤ 1000000000 + (↑an - ↑bn)
    --   ↔ ¬True ∨ ↑bn + ↑m ≤ 1000000000 + ↑an
    -- can be rewritten in the form (a ∨ b ↔ b)
    -- which becomes tautologically true if a is always false
    -- which Lean expresses as 'rewrite that as a → b, prove ¬a'
    simp only [not_true_eq_false, false_or,
               add_sub, le_sub_iff_add_le',
                or_iff_right_iff_imp]
    -- ⊢ 1000000000 + bs * 1000000000 + ↑an < bs * 1000000000 + ↑bn
    --   → ↑bn + ↑m ≤ 1000000000 + ↑an
    -- cancel the common term, bs * 1000000000
    rewrite [add_rotate, add_assoc, add_lt_add_iff_left]
    intro an_ll_bn
    -- we now have these premises:
    --   0 ≤ bn < 1000000000
    --   0 ≤ an
    --  bn > an + 1000000000
    -- which contradict, so we're done.
    linarith

theorem timespec_differences_equivalent
  : timespec_difference_at_least_mul = timespec_difference_at_least_cases
  := by
  funext a b m
  rewrite [Bool.eq_iff_iff]
  obtain ⟨m, m_bound⟩ := m
  obtain ⟨as, an, an_bound⟩ := a
  obtain ⟨bs, bn, bn_bound⟩ := b
  simp [timespec_difference_at_least_mul,
        timespec_difference_at_least_cases,
        timespec_to_ns,
        ONE_S_IN_NS]
  split_ifs with as_rel_bs
  . -- have as_rel_bs: as = bs
    simp [as_rel_bs]
    exact case_as_eq_bs an bn m
  . -- have as_rel_bs: ¬(as = bs)
    rcases lt_trichotomy as bs with as_lt_bs | _ | as_gt_bs
    . -- have as_lt_bs: as < bs
      obtain ⟨δs, rfl⟩ := Int.exists_add_of_le as_lt_bs
      exact case_as_lt_bs as δs an bn m an_bound
    . tauto -- as = bs already disposed of above
    . -- have as_gt_bs: bs < as
      -- this case must be split further into as = bs + 1 and as > bs + 1
      obtain ⟨δs, rfl⟩ := Int.exists_add_of_le as_gt_bs
      simp [x_ne_x_plus_stuff]
      cases δs with
      | zero =>
        exact case_as_eq_bs_plus_one bs an bn m bn_bound
      | succ q =>
        exact case_as_gt_bs_plus_one bs an bn m q m_bound bn_bound
