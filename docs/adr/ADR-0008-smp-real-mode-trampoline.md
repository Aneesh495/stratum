# ADR-0008: Real-Mode SMP Trampoline at Physical 0x8000

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
On x86-64 hardware, Application Processors (APs) begin execution in 16-bit real mode when awakened by the Bootstrap Processor (BSP) via the INIT-SIPI-SIPI inter-processor interrupt protocol.

## Decision
We place a dedicated real-mode assembly trampoline at physical address `0x8000`:
- SIPI vector 0x08 starts execution at `0x8000:0x0000`.
- The trampoline transitions through 16-bit real mode, loads temporary 32-bit GDT, enables CR4.PAE and IA32_EFER.LME, enables paging with CR3 loaded with the kernel PML4, and jumps into 64-bit long mode.
- Communication mailbox at physical `0x8F00` supplies the kernel PML4 pointer, initial per-CPU stack pointer, and entry C function pointer.

## Consequences
- Clean, deterministic AP startup supporting 1, 2, 4, and 8 vCPUs.
- No reliance on ACPI parking protocols or legacy BIOS tables.
