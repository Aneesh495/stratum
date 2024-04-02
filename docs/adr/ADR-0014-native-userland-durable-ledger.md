# ADR-0014: Native User Runtime, Interactive Shell, and Durable Ledger Service

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
A complete OS must run real userland programs that demonstrate concurrency, file I/O, IPC, cryptographic integrity, and network service delivery.

## Decision
We implement a native userland environment (`user/`):
- Freestanding C runtime (`crt0.S`, `syscall.c`, `printf.c`, `string.c`).
- Process 1 init supervisor (`user/apps/init.c`) running in Ring 3.
- Interactive command shell (`user/apps/sh.c`) supporting `help`, `echo`, `cat`, `ls`, `stat`, `mkdir`, `rm`, `sysinfo`, `bench`, `netstat`, `ping`, and `ledger`.
- Distributed durable ledger daemon (`user/apps/ledgerd.c`) utilizing SHA-256 Merkle trees, StrataFS transactional file persistence (`/strata/user_ledger.dat`), and TCP peer-to-peer replication over port 9090.

## Consequences
- Full userland verification in Ring 3.
- Complete multi-node cluster consensus across separate VM guests.
