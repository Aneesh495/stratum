# Performance experiments

`make bench` records a host-only microbenchmark in `build/host-benchmarks.log`.
It measures synthetic loops and memory operations. Guest syscall latency,
scheduler latency, filesystem I/O, and TCP throughput remain unmeasured by this
harness.


`make bench-guest` collects the same completed work on 1 and 4 vCPUs
with the same image, memory size and firmware. The harness records 30
fresh QEMU boot-to-shell samples after two warmups. Each profile also
records 30 short repetitions of counter overhead, native syscalls,
demand-zero faults, shared page allocation, runnable context switches, wake-to-run intervals, and exact pipe round trips. It records ten longer
repetitions of block reads, writes, fsync and fixed-work SMP scaling after three warmups. Ten
TCP transfers after two warmups are reconstructed from the Ethernet
capture. The host TCP client reaches the guest through QEMU user
networking.

Raw observations and input hashes are saved to `build/guest-bench/`.
Guest experiments report elapsed TSC ticks. Boot and TCP report host
monotonic nanoseconds. The harness reports medians and keeps every
sample. The short samples run on a pinned CPU. The scaling workload keeps
eight worker processes and 2,000,000 arithmetic operations fixed while
distributing the workers across the selected vCPUs. It includes launch
and wait overhead.

`make bench-compare BASELINE=<commit>` independently builds the baseline
and candidate and runs them in sequence on the same host. The baseline
uses only the candidate's benchmark program and benchmark harness as
an overlay. The comparison records the overlay hashes, both source
and image identities, every raw sample, and medians with interquartile
ranges. The result describes QEMU TCG performance in that environment.
