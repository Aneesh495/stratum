# ADR-0009: Per-CPU State Isolation via GS Base MSR

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Multi-core SMP kernels need fast, lockless access to core-local variables (current thread pointer, runqueue, Local APIC ID, CPU statistics, and core interrupt flags) without table lookups or hash calculations.

## Decision
We utilize the x86-64 `IA32_GS_BASE` MSR (`0xC0000101`):
- Each CPU initializes its own `cpu_local_t` structure.
- `wrmsr(0xC0000101, (uint64_t)&cpu_locals[cpu_id])` points the GS base directly to this structure.
- Per-CPU variables are accessed via `%gs:offset` assembly instructions or `this_cpu()` macros.
- Userland FS/GS swapping via `swapgs` guarantees that userland cannot modify kernel GS base state.

## Consequences
- O(1) single-cycle access to current thread and core runqueues.
- Complete hardware-enforced CPU isolation across all online cores.
