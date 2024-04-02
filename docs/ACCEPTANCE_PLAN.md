# Stratum Acceptance Plan and Capability Register

Version: 1.0.0
Status: Frozen targets. See [verification status](BUILD_STATUS.md) for observed coverage.

## 1. Overview
This document freezes the 64 capability families (K01 to K64) and the 12 fixed acceptance gates (A01 to A12) required for complete system verification. Source paths in the register describe intended subsystem ownership and can include files still to be implemented.

## 2. Capability Register (K01 to K64)

| ID | Capability Family | Primary Source Paths | Verification Mechanism |
| --- | --- | --- | --- |
| K01 | Original UEFI Loader | `boot/uefi/` | Clean boot of original PE/COFF loader into kernel entry point |
| K02 | Validated Boot Handoff | `kernel/core/boot.c` | Verification of magic, version, bounds, and memory map |
| K03 | Higher-Half Initialization | `kernel/arch/x86_64/paging.c` | PML4 switch to high kernel mapping without firmware calls |
| K04 | Exceptions and Interrupt Frames | `kernel/arch/x86_64/idt.c` | Fault dispatch, error code extraction, register dumping |
| K05 | GDT, TSS, and Protected Stacks | `kernel/arch/x86_64/gdt.c` | Ring 3 to Ring 0 transitions, IST double fault stack |
| K06 | ACPI Discovery and APIC Routing | `kernel/drivers/apic.c`, `acpi.c` | MADT parsing, local APIC and I/O APIC setup |
| K07 | Physical Page Allocation | `kernel/mm/pmm.c` | Bitmap buddy allocator, reservations, exhaustion checks |
| K08 | Kernel Object Allocation | `kernel/mm/slab.c` | Size-class slab allocator, alignment, zeroing, freeing |
| K09 | Four-Level Virtual Memory | `kernel/mm/vmm.c` | Page table walks, map, unmap, protection updates |
| K10 | Demand Paging | `kernel/mm/fault.c` | Page fault handler dynamically allocating anonymous pages |
| K11 | Copy-On-Write Address Spaces | `kernel/mm/cow.c` | Read-only shared mapping, write fault page duplication |
| K12 | SMP TLB Invalidation | `kernel/mm/tlb.c` | Inter-processor interrupt (IPI) broadcast and acknowledge |
| K13 | Shared Memory Objects | `kernel/mm/shm.c` | Named or anonymous shared physical pages across processes |
| K14 | Guarded Stacks and Permissions | `kernel/mm/guard.c` | Unmapped guard pages below stacks and W^X enforcement |
| K15 | AP Startup and Per-CPU State | `kernel/arch/x86_64/smp.c` | SIPI-INIT-SIPI sequence, per-CPU GS base, boot barrier |
| K16 | Atomic Ops and Spinlocks | `kernel/sync/spinlock.c` | Ticket or test-and-set spinlocks with IRQ state preservation |
| K17 | Sleeping Mutexes and Wait Queues| `kernel/sync/mutex.c`, `wait.c`| Thread descheduling, wait queue enrollment, race-free wake |
| K18 | Futex-Backed Synchronization | `kernel/sync/futex.c` | User-space address hashing, wait, wake, timeout |
| K19 | Preemptive SMP Scheduler | `kernel/sched/sched.c` | Per-CPU run queues, APIC timer preemption, fairness |
| K20 | Affinity and Load Distribution | `kernel/sched/load.c` | Work stealing, CPU affinity masks, thread migration |
| K21 | Monotonic Clocks and Deadlines | `kernel/sched/timer.c` | Calibrated APIC timer, min-heap timer queue, sleep |
| K22 | Idle and Deferred Work | `kernel/sched/idle.c` | `hlt` loop, bottom-half tasklet queue |
| K23 | Native ELF64 Loading | `kernel/proc/elf.c` | Parsing ELF headers, program headers, mapping segments |
| K24 | Process Lifecycle | `kernel/proc/process.c` | Fork, execve, exit, waitpid, zombie reaping |
| K25 | Native User Threads and TLS | `kernel/proc/thread.c` | Thread creation, stack allocation, FS base configuration |
| K26 | Safe Syscall Entry and Copying | `kernel/proc/syscall.c` | `syscall`/`sysretq`, pointer verification, copy_from_user |
| K27 | File Descriptor Lifecycle | `kernel/proc/fd.c` | Descriptor table, dup2, reference counts, close-on-exec |
| K28 | Pipes and Message Channels | `kernel/ipc/pipe.c`, `channel.c`| Bounded ring buffer, blocking read/write, EOF handling |
| K29 | Pollable Readiness | `kernel/ipc/poll.c` | Poll table registration, event notification on file/pipe/socket |
| K30 | Process Termination and Cleanup | `kernel/proc/exit.c` | Releasing VM, closing descriptors, notifying parent |
| K31 | UART and Console Input | `kernel/drivers/uart.c` | 16550A serial driver with interrupt-driven receive buffer |
| K32 | Framebuffer Text Terminal | `kernel/drivers/fb.c` | Linear framebuffer rendering with built-in 8x16 font |
| K33 | PCI Enumeration and MMIO | `kernel/drivers/pci.c` | Scanning PCI bus 0..255, BAR programming, MMIO mapping |
| K34 | Original Virtqueue Transport | `kernel/drivers/virtio_ring.c` | Split virtqueue descriptor, available, and used rings |
| K35 | Virtio Block I/O | `kernel/drivers/virtio_blk.c` | Block read, write, flush commands via virtqueue |
| K36 | Virtio Network I/O | `kernel/drivers/virtio_net.c` | Packet transmit and receive rings, header processing |
| K37 | Block Cache and Barriers | `kernel/block/cache.c` | LRU block buffer cache, dirty marking, device flushes |
| K38 | VFS and Mount Hierarchy | `kernel/fs/vfs.c` | Inode/dentry cache, pathname lookup, mount points |
| K39 | Initramfs and Tmpfs | `kernel/fs/initramfs.c`, `tmpfs.c`| Tar/cpio archive extraction, in-memory filesystem |
| K40 | Original StrataFS Disk Format | `kernel/fs/stratafs.c` | Superblock, inode table, direct and indirect block lookups |
| K41 | Filesystem Operations | `kernel/fs/strata_ops.c` | Create, read, write, seek, truncate, unlink, rename |
| K42 | Journal Transactions | `kernel/fs/journal.c` | Redo log recording, descriptor blocks, commit records |
| K43 | Fsync and Recovery | `kernel/fs/strata_fsync.c` | Flush ordering, replay of committed transactions on mount |
| K44 | Allocation and Corruption Recovery | `tools/strata_check.c` | Inode/block bitmap consistency, leak detection |
| K45 | Ethernet, ARP, IPv4, ICMP | `kernel/net/net_core.c` | Protocol demuxing, ARP resolution, ICMP echo responder |
| K46 | UDP Sockets | `kernel/net/udp.c` | Datagram endpoints, port binding, queue thresholds |
| K47 | TCP State Machine | `kernel/net/tcp.c` | 3-way handshake, reliable streaming, 4-way teardown |
| K48 | TCP Recovery and Flow Control | `kernel/net/tcp_flow.c` | Sliding window, RTO backoff, duplicate ACK fast retransmit |
| K49 | Bounded Protocol Resources | `kernel/net/net_buf.c` | Fixed buffer slab pool, explicit queue caps |
| K50 | Native User Runtime and SDK | `user/lib/` | C runtime startup (`_start`), syscall stubs, libc subset |
| K51 | Init and Process Supervision | `user/apps/init.c` | Process 1, child process reaping, daemon management |
| K52 | Interactive Shell and Utilities | `user/apps/sh.c`, `ls.c`, etc.| Command line parser, pipelines, redirection, file tools |
| K53 | Integrated Durable Service | `user/apps/ledgerd.c` | Concurrency, TCP protocol, StrataFS transactional ledger |
| K54 | Structured Per-CPU Tracing | `kernel/debug/trace.c` | Lockless ring buffer, event emission, loss accounting |
| K55 | Panic Diagnosis and Symbols | `kernel/debug/panic.c` | Register dump, stack trace, symbol table lookup |
| K56 | Independent Filesystem Checker | `tools/strata_check.c` | Host validation of raw StrataFS disk images |
| K57 | Deterministic Models and Replay | `tests/host/` | Host test harness verifying allocators and state machines |
| K58 | Reproducible Image Construction | `scripts/build_image.py` | Bit-for-bit identical kernel and disk image generation |
| K59 | Fault Injection and Fuzz Regression | `tests/faults/` | Memory failure injection, corrupted disk inputs |
| K60 | Real Guest Stress and Soak | `tests/guest/stress.c` | Multi-threaded workload across 8 CPUs and 1 GiB RAM |
| K61 | Measured Performance Experiments| `bench/` | Benchmarks for context switch, IPC, block and TCP throughput |
| K62 | Complete Technical Documentation | `docs/` | Architectural specs, ADRs, Mermaid diagrams |
| K63 | Source-Bound Acceptance Evidence | `scripts/run_acceptance.py` | Automated workload runner producing structured artifacts |
| K64 | Independent Evidence Verification| `scripts/verify_evidence.py` | Verification of logs, hashes, and negative controls |

## 3. Fixed Acceptance Workloads (A01 to A12)

- **A01: Boot and Image Validation**: 40 clean boots (1, 2, 4, 8 CPUs; 256M and 1G RAM; 5 repetitions each) + 5 degraded 64M boots + 128 malformed loader tests.
- **A02: Memory Management**: 2,000,000 deterministic host allocation/mapping operations; 50,000 guest demand/COW faults; 10,000 cross-CPU mapping replacements.
- **A03: Scheduling and Concurrency**: 1,000,000 task switches; 500 simultaneous user threads across >=32 processes on 4 and 8 CPUs; 10,000 wait/wake races.
- **A04: Lifecycle and ABI**: 10,000 fork/exec/wait cycles; 25,000 thread create/join cycles; 10,000 boundary syscall cases.
- **A05: IPC and Readiness**: 100,000 message/pipe transfers with partial I/O; 25,000 readiness/close races.
- **A06: Drivers and Block I/O**: 150,000 completed block requests; two full 16-bit virtqueue index wraps; 2,000 queue saturation and error tests.
- **A07: Filesystem and Recovery**: 100,000 mixed file/dir operations; 5,000 modeled crash states; 100 real VM interruption/reboot cycles.
- **A08: Networking**: 100,000 UDP datagrams; 1,000 completed TCP sessions; >=256 MiB verified TCP payload; 200 impaired sessions; 10,000 malformed packets.
- **A09: Integrated Service**: 2 guests, >=32 concurrent clients, 100,000 requests, 10,000 durable records, 10 server interruptions with recovery.
- **A10: Soak and Stability**: 60 minutes uninterrupted guest execution on 8 CPUs and 1 GiB RAM with active I/O, IPC, and zero memory leaks.
- **A11: Models and Fuzzing**: 2,000,000 completed fuzz executions across 6 targets (minimum 10 minutes per target).
- **A12: Reproducibility and Evidence**: 2 clean isolated builds with matching artifact hashes; complete verification with 12 negative controls.
