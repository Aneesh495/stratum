# ADR-0002: Modular Monolithic Kernel Architecture

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Operating systems require high runtime performance, direct hardware access, low context switch latency, and robust fault boundaries. Microkernel designs introduce substantial IPC overhead, while unstructured monolithic kernels suffer from tight coupling.

## Decision
We adopt a modular monolithic architecture for Stratum:
- Subsystems (PMM, VMM, SLAB, SMP, Sched, VFS, Net, Drivers) are isolated into distinct C modules with clear C interfaces.
- The kernel runs in Ring 0 with unified higher-half virtual address mapping.
- Ring 3 userland processes interact with the kernel exclusively through hardware-accelerated `syscall` / `sysretq` boundaries.

## Consequences
- Single address space for all kernel subsystems minimizes overhead.
- Clean header abstractions in `include/kernel/` maintain modularity.
- Substantive authored production code exceeds 10,000 lines without bloat.
