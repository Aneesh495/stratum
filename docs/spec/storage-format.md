# StrataFS v1 disk format

The format uses little-endian fields and 4,096-byte blocks. The active layouts are in [stratafs.h](../../include/kernel/stratafs.h) and [journal.h](../../include/kernel/journal.h).

| Blocks | Contents |
| --- | --- |
| 0 | Reserved |
| 1 | Filesystem superblock and CRC32 |
| 2 | Block allocation bitmap |
| 3 | Inode allocation bitmap |
| 4 to 67 | Inode table, 32 records of 128 bytes per block |
| 68 to 579 | Journal superblock and circular redo records |
| 580 onward | Directory, file, and indirect blocks |

Inode zero is reserved and inode one is the root. Inodes have 12 direct pointers and one 1,024-entry indirect block. Zero block pointers in regular files read as holes. Each directory block contains sixteen fixed 256-byte entries. A zero inode field marks a free slot. An inode generation protects stale in-memory references after reuse.

The journal publishes one pending transaction at a time. Commit writes a checksummed descriptor and block images, flushes, writes a commit record, and flushes again. It then persists the pending journal range before touching the home blocks. After checkpointing and flushing the home blocks, it clears the pending range and flushes the journal header. The filesystem serializes this sequence with its inode and bitmap updates.

Recovery validates the published descriptor, sequence, targets, commit record, and payload checksum before changing home blocks. A corrupt published transaction rejects the mount. Format-on-mount requires an all-zero volume. A legacy journal with matching head and tail can be upgraded. Invalid legacy filesystem inodes require offline inspection.

`scripts/test_recovery.py` tests a VM interruption after the published range is durable and before home writes. This is a QEMU process-interruption test. The storage device's flush behavior defines the durability boundary.
