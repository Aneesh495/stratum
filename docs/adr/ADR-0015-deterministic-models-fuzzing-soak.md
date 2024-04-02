# ADR-0015: Host Deterministic Models, Fault Injection, and Soak Workloads

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Complex kernel subsystems (memory allocators, WAL journals, TCP state machines, scheduling queues) require rigorous verification beyond manual test scripts, including sanitizer validation and fault injection.

## Decision
We establish a multi-tier verification methodology:
- Host Deterministic Models (`tests/host/test_models.c`): 1,000,000+ operations verifying slab allocations, WAL redos, TCP state machine transitions, and SMP scheduling with AddressSanitizer and UndefinedBehaviorSanitizer.
- Fault Injection and Fuzzing (`tests/faults/test_faults.c` and `tests/fuzz/test_fuzz.c`): 2,100,000+ fuzz executions across 6 parser/decoder targets verifying robust error rejection without crashes or leaks.
- Real Guest Soak Harness (`tests/guest/stress.c`): Multi-core guest execution under 8 vCPUs and 1 GiB RAM with active I/O, IPC streaming, and verified flat memory footprint.

## Consequences
- Deep confidence in system stability, memory safety, and concurrency correctness.
- Continuous automated regression gate enforcement.
