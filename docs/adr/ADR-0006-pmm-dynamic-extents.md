# ADR-0006: Physical Memory Management with Dynamic Extents

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Fixed-size physical page arrays allocated for multi-gigabyte memory limits fail on low-memory VMs (such as 64 MiB degraded testing profiles), causing early boot memory exhaustion.

## Decision
We dynamically scale the physical page frame metadata array:
- PMM walks the UEFI memory map and identifies conventional RAM regions.
- The metadata array (`page_t` structs) is dynamically scoped to the maximum accessible physical page frame of the system.
- Allocations utilize an efficient bitmap buddy allocator supporting single-page and multi-page order allocations with strict spinlock synchronization.

## Consequences
- Clean boot across all memory configurations: 64 MiB, 256 MiB, and 1 GiB.
- Low static overhead in memory-constrained environments.
