# ADR-0003: Versioned Boot Handoff Contract

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
A major source of kernel boot failures is undocumented, loose coupling between the firmware bootloader and kernel entry point.

## Decision
We enforce a strictly typed, versioned boot contract struct `boot_handoff_t`:
- Signature magic: `0x5354524154554D31` ("STRATUM1").
- Contract version: `0x00010000`.
- Struct size: exactly 256 bytes with 64-bit alignment and reserved padding.
- Required fields: kernel physical and virtual entry points, physical memory map descriptor pointer and entry count, GOP linear framebuffer parameters, ACPI RSDP pointer, and initramfs physical address and length.

## Consequences
- The kernel entry code validates the magic, version, and bounds before dereferencing any pointers.
- Malformed loader invocations are caught immediately at entry step 1 with explicit diagnostic rejection.
