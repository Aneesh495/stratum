# Verification status

The current verification baseline covers the normal QEMU image and an opt-in diagnostic boot. The [full acceptance plan](ACCEPTANCE_PLAN.md) remains open. `make acceptance` and `make release` fail while those workloads remain unverified. `make evidence` generates source-bound regression evidence under `build/evidence/`.

| Gate | Exercised in regression | Still required |
| --- | --- | --- |
| A01 | 45 fresh boots with per-CPU work, 17 malformed EFI images, 111 debugger-injected handoff mutations, and entry register inspection | Release evidence binding |
| A02 | 2 million host model operations, 51,200 reconciled guest demand-zero allocations, fork/COW protection, and 10,000 shared kernel mapping replacements on each of 4 and 8 CPUs | Shared user mappings, broader failure injection and ownership checks |
| A03 | 512 simultaneous user workers across 32 processes, over 1 million runnable user switches, and 10,000 futex cases on each of 4 and 8 CPUs | Broader fairness and process-wide cancellation checks |
| A04 | Ring 3 entry, 10,000 fork/exec/wait cycles, 25,000 thread joins with TLS and SSE state, and 10,000 boundary calls on each of 4 and 8 CPUs | Process-wide cancellation and broader boundary and reaping checks |
| A05 | 100,000 verified pipe and channel transfers, 25,000 readiness races, backpressure, EOF, and descriptor generation checks on each of 4 and 8 CPUs | Process-wide cancellation and broader socket readiness races |
| A06 | 150,000 real virtio-blk reads and two full block-queue wraps, and two full wraps of each virtio-net queue over an independent Ethernet link, 500 block-queue saturations, 500 real PCI resets, and 1,000 backend I/O errors | Cold-device workload and network-driver error and timeout recovery |
| A07 | 128,006 mixed operations, 5,000 modeled crashes, 1,500 corrupt model states, 100 guest stops across ten WAL stages, and independent namespace traversal | Directory deletion and rename, links, open-unlinked lifetimes, block cache pressure and fault injection |
| A08 | 150,000 verified UDP echoes over Ethernet, guest TCP data transfer including a retransmitted reply, and a separate host-client path | Bulk TCP transfers, broader impairment sessions, and malformed traffic campaign |
| A09 | Native TCP service, durable acknowledgment, duplicate/conflict handling, and a record transferred to a second guest and read after reboot | Concurrent clients, automatic replication, workload counts, and repeated interruption |
| A10 | Five bounded guest stress cycles and a 189-round mixed workload probe on 8 vCPUs and 1 GiB | Full 60-minute mixed workload and final evidence binding |
| A11 | 2.1 million standalone parser-model mutations, a sanitizer probe of six production parsers, and independent wait/TLB exploration with negative controls | Full production-target fuzzing duration and source-bound evidence |
| A12 | Source-bound regression bundle, 14 verifier mutation controls, and two isolated artifact builds | Complete fixed-gate evidence, full verifier corpus, and release publication |

The original smoke reports and synthetic benchmark table were removed from the current tree. Git history preserves them. The [host benchmark harness](../bench/results.md) records raw host measurements separately from guest results.

## Compatibility

StrataFS checks the superblock and root inode before mounting. A disk with an invalid legacy root inode is left intact and requires offline inspection. The shell remains available if the root mount fails. Unlinking an open file returns `EBUSY`, and directory deletion and directory rename are pending.

Run the wire campaign with `make test-net-wrap`. Run the 10-minute production parser campaign with `make fuzz-production` from a clean commit. It preserves the starting seeds, discovered corpus, sanitizer logs, and execution counts under `build/`.

`make test-drivers` also verifies block-queue saturation, device reset, and backend I/O errors. The block driver waits for reset acknowledgment before freeing old DMA queues. It returns the original error to the caller and never automatically repeats a timed-out write. [Virtio device reset contract](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html) · [QEMU block error injection](https://www.qemu.org/docs/master/devel/testing/blkdebug.html)

`make soak` runs the mixed workload for at least an hour. It uses 8 vCPUs and 1 GiB in one VM and preserves per-round resource checks, serial logs, a packet capture, and the final filesystem check.
