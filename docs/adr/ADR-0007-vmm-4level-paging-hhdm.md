# ADR-0007: Virtual Memory 4-Level Paging and Higher-Half Direct Map

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Kernel address space design must provide access to arbitrary physical memory without repeated page-table modifications, while isolating userland virtual addresses and enforcing memory protection.

## Decision
We implement x86-64 4-level paging (PML4, PDPT, PD, PT):
- Higher-Half Direct Map (HHDM) is established at base virtual address `0xFFFF800000000000`, mapping all physical RAM directly with read/write supervisor permissions.
- Kernel code and static data mapped at `0xFFFFFFFF80000000`.
- Userland address space restricted to lower canonical half (`0x0000000000000000` to `0x00007FFFFFFFFFFF`).
- Demand paging and Copy-On-Write (COW) handle user page faults (vector 14) dynamically.

## Consequences
- Physical address conversion in kernel code is a simple pointer addition `phys + HHDM_BASE`.
- Full separation of user and kernel address spaces with NX (No-Execute) and W^X enforcement.
