# Performance experiments

`make bench` records a host-only microbenchmark in `build/host-benchmarks.log`.
It measures synthetic loops and memory operations. Guest syscall latency,
scheduler latency, filesystem I/O, and TCP throughput remain unmeasured by this
harness.
