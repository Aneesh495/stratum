# Verification status

`make acceptance` builds and runs the fixed campaigns from a clean checkout.
It keeps raw artifacts, file hashes, command observations, and individual
capability results under `build/acceptance/`. `make verify EVIDENCE=path`
requires complete evidence. `make evidence` runs the smaller regression
suite and verifies reproducibility in two isolated builds.

Stratum v0.1.0 completed all 12 acceptance gates. The tagged execution
commit is `f0a3dc824a8867e7f29bafe9fb76e434b726db64`. All 40 fixed
commands passed on the hosted Ubuntu runner.

[Full hosted acceptance](https://github.com/Aneesh495/stratum/actions/runs/42530758442) · [Standard CI](https://github.com/Aneesh495/stratum/actions/runs/42530752144) · [Release](https://github.com/Aneesh495/stratum/releases/tag/v0.1.0)

| Gate | Verified coverage |
| --- | --- |
| A01 | 45 fresh boots, 128 rejected inputs, debugger entry observations, PS/2 input, exact framebuffer glyphs, and nested protected stacks on 1, 4 and 8 CPUs |
| A02 | 2 million model operations, 51,200 guest demand faults, 10,000 mapping replacements per CPU profile, shared mapping and COW tests, real 64 MiB allocator exhaustion, slab rollback, and object reuse |
| A03 | 512 simultaneous workers across 32 processes on each of 4 and 8 CPUs, over a million runnable switches per profile, 10,000 futex cases, bounded worker completion, and 4,096 deadline checks with observed CPU migrations per profile |
| A04 | 10,000 fork/exec/wait cycles, 25,000 thread cycles, 10,000 boundary cases, 10,008 targeted cancellations, 256 group exits on each of 4 and 8 CPUs, and native shell supervision on two memory profiles |
| A05 | 100,000 verified transfers, 25,000 readiness races, 2,500 socket poll races, and 32 orphan reaps per profile |
| A06 | Block and network index wraps, 500 block saturations, 500 PCI resets, 1,000 backend errors, real MSI-X completions, and bounded timeout recovery |
| A07 | 128,006 mixed operations, 5,000 modeled crashes, 100 VM interruptions, independent namespace checks, cache pressure, allocation rollback, pinned dirty copies under cache pressure, and injected flush failure with remount and retry |
| A08 | 150,000 exact UDP echoes, 1,000 TCP sessions, 316,211,200 TCP bytes, 200 impaired sessions, and 10,000 invalid frames. Separate direct Linux interoperability, deterministic fast retransmission, ICMP and socket capacity tests |
| A09 | 32 native clients, 100,000 requests, 10,000 records checked on both disks, and ten primary interruptions |
| A10 | 11,056 mixed rounds in 3,600.1 seconds on 8 vCPUs and 1 GiB, with quiescent resources reconciled |
| A11 | 523,692,845 completed production parser mutations across six targets, each for at least 600 seconds |
| A12 | Matched baseline benchmark, two reproducible builds, 13 full verifier controls, and byte-for-byte verification of the public release downloads |

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


A frozen run of `92e2c38` completed every workload. The newer evidence
verifier checked its retained raw observations. That run failed the
cancellation page-accounting and fast retransmission workloads. The
following candidate checks physical pages together with slab-owned pages
and separately compares live objects. The TCP test duplicates a genuine
gap acknowledgment after a controlled loss.

`make test-entry-stacks test-resource-limits test-supervision` checks
nested interrupt stacks, low-memory allocator rollback, TCP socket
capacity, and init restarting a reaped shell on the same boot.
