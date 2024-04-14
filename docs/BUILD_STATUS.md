# Verification status

The current verification baseline covers the normal QEMU image and an opt-in diagnostic boot. The [full acceptance plan](ACCEPTANCE_PLAN.md) remains open. `make acceptance` and `make release` fail while those workloads remain unverified. `make evidence` generates source-bound regression evidence under `build/evidence/`.

| Gate | Exercised in regression | Still required |
| --- | --- | --- |
| A01 | Nine QEMU profiles, 17 malformed EFI images, and a shell with no data disk | Repeated boots, per-CPU workload probes, debugger evidence, and the remaining malformed loader and handoff cases |
| A02 | 2 million host model operations, concurrent guest allocator checks, 51,200 reconciled demand-zero allocations across four diagnostic profiles, and fork/COW protection | Cross-CPU mapping replacement and the remaining memory oracles |
| A03 | Preemption and workers on the SMP diagnostic path | User-thread API, live population and race campaigns |
| A04 | Ring 3 entry, 64 fork/reap cycles per diagnostic profile, invalid status pointer, and a faulting child | Exec, native threads, TLS, and the full boundary corpus |
| A05 | Pipe transfer, readiness, EOF, and descriptor duplication | Message channels and full partial-I/O and readiness-race workloads |
| A06 | 150,000 real virtio-blk reads and two full block-queue wraps, plus virtio-net frames on the wire | Network queue wraps, cold-device workloads, saturation, and reset recovery |
| A07 | Directory enumeration, atomic regular-file rename, indirect-block I/O, 100 stops across ten WAL stages, and corrupt-image rejection | Full mixed workload, modeled crash-state count, and independent namespace traversal |
| A08 | Guest TCP data transfer, real ARP, exact Ethernet capture payloads including a retransmitted reply, and a separate host-client path | Bulk and broader impairment sessions, malformed traffic campaign, and UDP interoperability |
| A09 | Native TCP service, durable acknowledgment, duplicate/conflict handling, and a record transferred to a second guest and read after reboot | Concurrent clients, automatic replication, workload counts, and repeated interruption |
| A10 | Five bounded guest stress cycles, including 8 vCPUs and 1 GiB | 60 minutes of mixed active workloads and full resource reconciliation |
| A11 | 2.1 million standalone parser-model mutations under sanitizers | Production-target fuzzing duration and independent wait/TLB exploration |
| A12 | Source-bound regression bundle, 14 verifier mutation controls, and two isolated artifact builds | Complete fixed-gate evidence, full verifier corpus, and release publication |

The original smoke reports and synthetic benchmark table were removed from the current tree. Git history preserves them. The [host benchmark harness](../bench/results.md) records raw host measurements separately from guest results.

## Compatibility

StrataFS checks the superblock and root inode before mounting. A disk with an invalid legacy root inode is left intact and requires offline inspection. The shell remains available if the root mount fails. Unlinking an open file returns `EBUSY`, and directory deletion and directory rename are pending.
