# Verification status

The current verification baseline covers the normal QEMU image and an opt-in diagnostic boot. The [full acceptance plan](ACCEPTANCE_PLAN.md) remains open. `make acceptance` and `make release` fail while those workloads remain unverified. `make evidence` generates source-bound regression evidence under `build/evidence/`.

| Gate | Exercised in regression | Still required |
| --- | --- | --- |
| A01 | 45 fresh boots with per-CPU work, 17 malformed EFI images, 111 debugger-injected handoff mutations, and entry register inspection | Release evidence binding |
| A02 | 2 million host model operations, 51,200 reconciled guest demand-zero allocations, fork/COW protection, and 10,000 shared kernel mapping replacements on each of 4 and 8 CPUs | Shared user mappings, broader failure injection and ownership checks |
| A03 | 512 simultaneous user workers across 32 processes, over 1 million runnable user switches, and 10,000 futex cases on each of 4 and 8 CPUs | Broader fairness and process-wide cancellation checks |
| A04 | Ring 3 entry, 10,000 fork/exec/wait cycles, 25,000 thread joins with TLS and SSE state, and 10,000 boundary calls on each of 4 and 8 CPUs | Process-wide cancellation and broader boundary and reaping checks |
| A05 | 100,000 verified pipe and channel transfers, 25,000 readiness races, backpressure, EOF, and descriptor generation checks on each of 4 and 8 CPUs | Process-wide cancellation and broader socket readiness races |
| A06 | 150,000 real virtio-blk reads and two full block-queue wraps, plus virtio-net frames on the wire | Network queue wraps, cold-device workloads, saturation, and reset recovery |
| A07 | Directory enumeration, atomic regular-file rename, indirect-block I/O, 100 stops across ten WAL stages, and corrupt-image rejection | Full mixed workload, modeled crash-state count, and independent namespace traversal |
| A08 | Guest TCP data transfer, real ARP, exact Ethernet capture payloads including a retransmitted reply, and a separate host-client path | Bulk and broader impairment sessions, malformed traffic campaign, and UDP interoperability |
| A09 | Native TCP service, durable acknowledgment, duplicate/conflict handling, and a record transferred to a second guest and read after reboot | Concurrent clients, automatic replication, workload counts, and repeated interruption |
| A10 | Five bounded guest stress cycles, including 8 vCPUs and 1 GiB | 60 minutes of mixed active workloads and full resource reconciliation |
| A11 | 2.1 million standalone parser-model mutations under sanitizers and independent wait/TLB exploration with negative controls | Production-target fuzzing duration, corpus and sanitizer evidence |
| A12 | Source-bound regression bundle, 14 verifier mutation controls, and two isolated artifact builds | Complete fixed-gate evidence, full verifier corpus, and release publication |

The original smoke reports and synthetic benchmark table were removed from the current tree. Git history preserves them. The [host benchmark harness](../bench/results.md) records raw host measurements separately from guest results.

## Compatibility

StrataFS checks the superblock and root inode before mounting. A disk with an invalid legacy root inode is left intact and requires offline inspection. The shell remains available if the root mount fails. Unlinking an open file returns `EBUSY`, and directory deletion and directory rename are pending.
