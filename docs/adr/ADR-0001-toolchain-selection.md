# ADR-0001: Pinned LLVM Toolchain Selection

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Building a modern x86-64 freestanding operating system from scratch requires a predictable, reproducible compiler and linker toolchain capable of generating both UEFI PE32+ binaries and freestanding 64-bit ELF kernel executables without host C runtime pollution.

## Decision
We standardized on LLVM Clang and LLD with NASM.
- UEFI bootloader target: `x86_64-unknown-windows` with `-fuse-ld=lld-link` or LLD.
- Kernel target: `x86_64-unknown-none-elf` with `-mcmodel=kernel`, `-mno-red-zone`, `-ffreestanding`, and `-fno-stack-protector`.
- Assembler: NASM for pure 16-bit real-mode and early 64-bit trampolines.

## Consequences
- Guaranteed reproducible compilation across macOS host environments.
- Direct PE/COFF and ELF64 artifact generation without cross-binutils dependencies.
- Strict freestanding compliance preventing accidental glibc or host runtime linkages.
