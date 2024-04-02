# ADR-0005: 64-Bit GDT, TSS, and Interrupt Stack Tables (IST)

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
When kernel stack overflows occur or double faults (#DF) are raised, executing exception handlers on the existing corrupted stack causes immediate triple faults and silent CPU resets.

## Decision
We configure a 64-bit Global Descriptor Table (GDT) and Task State Segment (TSS) for every CPU:
- GDT entries: Null descriptor, 64-bit Kernel Code (0x08), Kernel Data (0x10), User Data (0x18), User Code (0x20), and 16-byte TSS descriptor (0x28).
- Dedicated IST1 (Double Fault) and IST2 (NMI) stacks mapped with guard pages.
- RSP0 in TSS set to the kernel stack for safe Ring 3 to Ring 0 transitions.

## Consequences
- Guaranteed deterministic recovery and diagnostic register dumping during double faults and CPU traps.
- Complete hardware stack switching upon privilege level elevation.
