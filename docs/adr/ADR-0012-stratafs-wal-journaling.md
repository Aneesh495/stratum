# ADR-0012: StrataFS On-Disk Structure and Write-Ahead Logging (WAL)

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Filesystem persistence must survive unexpected power loss or virtual machine termination without metadata inconsistency or orphaned data blocks.

## Decision
We implement StrataFS with an atomic Write-Ahead Logging (WAL) journal:
- Disk layout: Block 0 (boot sector), Block 1 (StrataFS Superblock), Block 2 (Block Bitmap), Block 3 (Inode Bitmap), Blocks 4..67 (Inode Table), Blocks 68..195 (WAL Journal ring), Blocks 196+ (Data Blocks).
- Superblock magic `0x53545241` ("STRA") with 4096-byte blocks.
- WAL journal circular buffer records metadata updates with sequence numbers, transaction IDs, block counts, and 32-bit CRC checksums.
- During mount, StrataFS inspects the WAL journal and replays uncommitted transactions before opening user files.

## Consequences
- Guaranteed crash consistency across reboots.
- Rapid recovery from ungraceful VM shutdowns without scanning the entire disk volume.
