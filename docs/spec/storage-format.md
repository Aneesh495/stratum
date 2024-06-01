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
File and directory renames commit as one transaction. Moving a directory across
parents changes its `..` entry and both parents' link counts. The move rejects
cycles, and replacement of a directory requires an empty closed destination.
`rmdir` removes an empty closed directory. Hard links name the same regular
inode. Symbolic links store an absolute path of up to 255 bytes. Resolution
follows at most eight links. The directory entry type for a symbolic link is 3.

Unlinking an open file, or replacing its last name by rename, sets the inode's
orphan flag and clears its link count in the namespace transaction. Open
descriptors retain the inode until final close. After journal replay, mounting
reclaims orphan inodes only after checking that no directory still references
them. The independent checker expects this recovery to finish before it checks
a volume interrupted with open unlinked files.

The journal publishes one pending transaction at a time. Commit writes a checksummed descriptor and block images, flushes, writes a commit record, and flushes again. It then persists the pending journal range before touching the home blocks. After checkpointing and flushing the home blocks, it clears the pending range and flushes the journal header. The filesystem serializes this sequence with its inode and bitmap updates.

Recovery validates the published descriptor, sequence, targets, commit record, and payload checksum before changing home blocks. A corrupt published transaction rejects the mount. Format-on-mount requires an all-zero volume. A legacy journal with matching head and tail can be upgraded. Invalid legacy filesystem inodes require offline inspection.

`scripts/test_recovery.py` tests a VM interruption after the published range is durable and before home writes. This is a QEMU process-interruption test. The storage device's flush behavior defines the durability boundary.
`make test-storage-crash` repeats ten interruption points ten times each and
records the expected old or new record, replay result, and disk hashes.

The offline checker walks inode parent relationships back to root. It
checks namespace reachability, directory cycles, duplicate names, entry
types, dot entries, link counts, block ownership, and allocation counts.
`make test-storage-mixed` runs 128,006 checked operations against a
disposable volume, reboots it, verifies a durable fixture, and checks
that six deliberately corrupted copies are rejected. The separate WAL
model exercises 5,000 simulated crash states and 1,500 corrupted states.

`make test-storage-namespace` checks directory moves, link identity, bounded symbolic links, and open-unlinked files across guest interruptions.
