# Performance experiments

`make bench` records a host-only microbenchmark in `build/host-benchmarks.log`.
It measures synthetic loops and memory operations. Guest syscall latency,
scheduler latency, filesystem I/O, and TCP throughput remain unmeasured by this
harness.


`make bench-guest` collects the same completed work on 1 and 4 vCPUs
with the same image, memory size and firmware. The harness records 30
fresh QEMU boot-to-shell samples after two warmups. Each profile also
records 30 short repetitions of counter overhead, native syscalls,
demand-zero faults and exact pipe round trips. It records ten longer
repetitions of block reads, writes and fsync after three warmups. Ten
TCP transfers after two warmups are reconstructed from the Ethernet
capture. The host TCP client reaches the guest through QEMU user
networking.

Raw observations and input hashes are saved to `build/guest-bench/`.
Guest experiments report elapsed TSC ticks. Boot and TCP report host
monotonic nanoseconds. The harness reports medians and keeps every
sample. The single pinned guest worker and the sequential TCP stream
do not measure parallel throughput or physical-core scaling.
