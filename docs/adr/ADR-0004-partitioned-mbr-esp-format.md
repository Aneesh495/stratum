# ADR-0004: Partitioned MBR ESP Format for UEFI Boot Disks

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
UEFI firmware requires an EFI System Partition (ESP) formatted as FAT32. Some virtual machines and firmware environments strictly require a partition table (either GPT or MBR) rather than an unpartitioned "superfloppy" FAT32 image.

## Decision
We format the 64 MiB boot image with a hybrid MBR partition table:
- Sector 0 contains a standard MBR with Partition 1 set to type 0xEF (EFI System Partition), starting at LBA 2048 (1 MiB offset).
- The FAT32 filesystem resides inside Partition 1 at `image@@1M`.
- Standard UEFI directory layout `::/EFI/BOOT/BOOTX64.EFI` is populated alongside `startup.nsh` and `stratum.elf`.

## Consequences
- 100% boot compatibility across QEMU OVMF UEFI configurations.
- Clear separation between partition table metadata and filesystem payloads.
