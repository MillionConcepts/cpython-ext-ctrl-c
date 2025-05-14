//! This crate exists to test the equivalence of the two versions of
//! `timespec_difference_at_least` from `CheckSignalsOftenEnough.c`.
//!
//! When used as a library crate, it provides safe wrappers for both
//! versions of that function; however, its _purpose_ is to be a
//! container for the unit test at the bottom of this file.
//!
//! Note that this crate _actually_ tests the code in `tdal.c`, not
//! the code in `../../CheckSignalsOftenEnough.c`.  Presently the
//! two copies of this code must be kept in sync manually.

use core::ptr;
use libc::{c_int, c_long, timespec};

const ONE_S_IN_NS: u32 = 1_000_000_000;

unsafe extern "C" {
    /// The SIMPLE_TIMESPEC_DIFFERENCE version
    fn c_timespec_difference_at_least_mul(
        after: *const timespec,
        before: *const timespec,
        min_ns: c_long,
    ) -> c_int;

    /// The !SIMPLE_TIMESPEC_DIFFERENCE version
    fn c_timespec_difference_at_least_cases(
        after: *const timespec,
        before: *const timespec,
        min_ns: c_long,
    ) -> c_int;
}

/// Safe wrapper for c_timespec_difference_at_least_mul.
pub fn timespec_difference_at_least_mul(
    after: &timespec,
    before: &timespec,
    min_ns: u32,
) -> bool {
    assert!(min_ns < ONE_S_IN_NS, "min_ns must be less than 1 second");
    // SAFETY: we are passing valid pointers to initialized timespec
    // structs; we checked that min_ns is in the valid range.
    let rv = unsafe {
        c_timespec_difference_at_least_mul(
            ptr::from_ref(after),
            ptr::from_ref(before),
            min_ns.into(),
        )
    };
    rv != 0
}

/// Safe wrapper for c_timespec_difference_at_least_cases.
pub fn timespec_difference_at_least_cases(
    after: &timespec,
    before: &timespec,
    min_ns: u32,
) -> bool {
    assert!(min_ns < ONE_S_IN_NS, "min_ns must be less than 1 second");
    // SAFETY: we are passing valid pointers to initialized timespec
    // structs; we checked that min_ns is in the valid range.
    let rv = unsafe {
        c_timespec_difference_at_least_cases(
            ptr::from_ref(after),
            ptr::from_ref(before),
            min_ns.into(),
        )
    };
    rv != 0
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    prop_compose! {
        /// This Strategy function is designed to ensure that we hit
        /// all four of the cases in the 'cases' version of t_d_a_l:
        ///
        ///   after.tv_sec < before.tv_sec
        ///   after.tv_sec = before.tv_sec
        ///   after.tv_sec = before.tv_sec + 1
        ///   after.tv_sec > before.tv_sec + 1
        ///
        /// The range of the generated tv_sec values are constrained
        /// to ±2²⁴ to keep the search space reasonable; neither
        /// version of the function has a strong dependence on the
        /// absolute magnitude of either input's tv_sec field.
        ///
        /// We cannot return a pair of timespec structs because
        /// libc::timespec does not implement Debug, so instead
        /// we return a pair of pairs and convert to timespec
        /// structs in the actual test function.
        fn arb_timespec_pair()(
            sec_before in -16_777_216 ..= 16_777_216,
            sec_delta in prop_oneof![
                Just(0),
                Just(1),
                -16_777_216_i32 ..= -1,
                2_i32 ..= 16_777_216,
            ],
            ns_after in 0u32..ONE_S_IN_NS,
            ns_before in 0u32..ONE_S_IN_NS,
        ) -> ((i32, u32), (i32, u32)) {
            (
                (sec_before + sec_delta, ns_after),
                (sec_before, ns_before)
            )
        }
    }

    proptest! {
        #[test]
        fn timespec_differences_equivalent(
            ((s_after, ns_after),
             (s_before, ns_before)) in arb_timespec_pair(),
            min_ns in 0..ONE_S_IN_NS
        ) {
            let after = timespec {
                tv_sec: s_after.into(),
                tv_nsec: ns_after.into(),
            };
            let before = timespec {
                tv_sec: s_before.into(),
                tv_nsec: ns_before.into(),
            };
            let m = timespec_difference_at_least_mul(&after, &before, min_ns);
            let c = timespec_difference_at_least_cases(&after, &before, min_ns);
            assert_eq!(
                m, c,
                "mismatch: after={}.{:09} before={}.{:09} min_ns=0.{:09} mul={} cases={}",
                after.tv_sec, after.tv_nsec,
                before.tv_sec, before.tv_nsec,
                min_ns, m, c
            );
        }
    }
}
