#pragma once
///@file
///
/// Cross-thread waiting machinery for the parallel evaluator.
///
/// When several threads force the same thunk, one of them claims it
/// (see ThunkState::Evaluating in value.hh) and evaluates; the others
/// transition it to Awaited and block here until the winner publishes
/// the result. Waiters are woken via a small array of sharded
/// condition variables, keyed by thunk address (same design as
/// Determinate Nix's waiter domains).

#include "lix/libexpr/value.hh"

namespace nix {

/**
 * Unique id for the current thread, allocated lazily on first use by
 * the evaluator. Threads that never evaluate never get an id.
 */
uint32_t getMyEvalThreadId();

/**
 * Block until the given thunk leaves the Awaited state (i.e. its
 * state word is no longer Awaited). The caller must have already
 * transitioned the thunk to Awaited. Returns when the state changes;
 * the caller re-reads it. Also checks for interrupts so a blocked
 * waiter unwinds on SIGINT.
 */
void waitOnThunk(Value::Thunk & thunk);

/**
 * Wake any threads waiting on the given thunk. Must be called after
 * publishing a result (or reverting to Unevaluated on error) whenever
 * the previous state was Awaited.
 */
void notifyThunkWaiters(Value::Thunk & thunk);

/**
 * Wake all threads blocked in waitOnThunk, regardless of thunk. Used
 * on interrupt so waiters can check their interrupt state.
 */
void wakeAllThunkWaiters();

} // namespace nix
