# Verification status

`make acceptance` builds and runs the fixed campaigns from a clean checkout.
It keeps raw artifacts, file hashes, command observations, and individual
capability results under `build/acceptance/`. `make verify EVIDENCE=path`
requires complete evidence. `make evidence` runs the smaller regression
suite and verifies reproducibility in two isolated builds.

| Gate | Observed coverage | Remaining release work |
| --- | --- | --- |
| A01 | 45 fresh boots, 128 rejected loader and handoff inputs, debugger entry observations, PS/2 input, and exact framebuffer glyphs | Combined run on final source |
| A02 | 2 million model operations, 51,200 guest demand faults, 10,000 mapping replacements per CPU profile, shared mapping and COW tests, and real 64 MiB allocator exhaustion | Combined run on final source |
| A03 | 512 simultaneous workers across 32 processes on each of 4 and 8 CPUs, over a million runnable switches per profile, 10,000 futex cases, and bounded worker completion | Combined run on final source |
| A04 | 10,000 fork/exec/wait cycles, 25,000 thread cycles, 10,000 boundary cases, 10,008 targeted cancellations, and 256 group exits on each of 4 and 8 CPUs | Combined run on final source |
| A05 | 100,000 verified transfers, 25,000 readiness races, 2,500 socket poll races, and 32 orphan reaps per profile | Combined run on final source |
| A06 | Block and network index wraps, 500 block saturations, 500 PCI resets, 1,000 backend errors, real MSI-X completions, and bounded timeout recovery | Combined run on final source |
| A07 | 128,006 mixed operations, 5,000 modeled crashes, 100 VM interruptions, independent namespace checks, cache pressure, and real allocation rollback | Combined run on final source |
| A08 | 150,000 exact UDP echoes, 1,000 TCP sessions, 316,211,200 TCP bytes, 200 impaired sessions, and 10,000 invalid frames on `cdcc41f`. Separate direct Linux interoperability and sliding-window recovery tests | Combined run on final TCP implementation |
| A09 | 32 native clients, 100,000 requests, 10,000 records checked on both disks, and ten primary interruptions on `cdcc41f` | Combined run on final source |
| A10 | Mixed workload on 8 vCPUs and 1 GiB with per-round resource reconciliation | Verified one-hour run on final source |
| A11 | 1,470,633,393 completed production parser mutations across six targets on `cdcc41f`, each for at least 600 seconds | Combined run on final source |
| A12 | Two isolated artifact builds and 14 regression verifier controls. Native performance harness, full workload verifier, and capability-to-artifact bindings | Open capability proofs, completed full verifier corpus, and hosted release |

The original smoke reports and synthetic benchmark table are preserved in Git
history. The [benchmark harness](../bench/results.md) keeps host and guest
measurements separate.

## Run individual campaigns

- `make test-framebuffer` enables the framebuffer mirror and PS/2 input with
  `console=both` in the boot profile. The serial shell also remains available.
- `make test-cancel` checks native cancellation, group exit, socket races,
  orphan reaping, and resource accounting.
- `make test-storage-cache test-block-timeout test-tmpfs` checks cache
  eviction and rollback, delayed DMA with reset, and volatile file lifetimes.
- `make test-tcp-campaign test-tcp-recovery test-tcp-reference` checks exact
  TCP payloads from captures. See the [reference guest requirements](spec/network-contract.md).
- `make soak` checks progress, packet delivery, resource baselines, and the
  final filesystem over a continuous 60-minute run.
- `make test-service` exercises 32 clients and synchronous replication over
  isolated Ethernet, verifies both disks, and restarts the primary ten times.
- `make fuzz-production` uses the six production parsers and preserves
  seeds, discovered corpus, sanitizer logs, and execution counts.

StrataFS leaves an invalid legacy volume intact for offline inspection and
keeps the shell available. The block driver waits for device reset
acknowledgment before releasing DMA queues. The guest caller decides whether
to retry. [Virtio device reset contract](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
