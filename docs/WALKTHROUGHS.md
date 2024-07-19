# Execution walkthroughs

## First user instruction

The [UEFI loader](../boot/uefi/main.c) stages the kernel's executable
segments and the initramfs, snapshots the firmware memory map, and
retries `ExitBootServices` with a new map if the firmware key changes.
After leaving firmware, [`_start`](../kernel/arch/x86_64/entry.S) hands
control to `kmain`. [`boot_validate_handoff`](../kernel/core/boot_check.c)
checks the structure, memory ranges and ownership before the kernel
allocates or uses passed buffers. An invalid handoff stops the boot
with a serial diagnostic.

`start_system` initializes the devices and mounts the archive before
`user_elf_load` builds the isolated init image. `process_create_init`
owns its descriptor table and kernel stack. `user_process_starter`
enters ring 3. Init then forks and execs `/bin/sh`, which writes the first
prompt. The boot campaign records the linked entry, handoff registers,
rejection cases and per-CPU work. [Boot campaign](../scripts/test_boot_campaign.py) · [Native init](../user/apps/init.c)

## Preempted task

The local APIC delivers a timer interrupt to `sched_tick`.
The bootstrap CPU advances the global monotonic clock. The scheduler
checks runnable priority and the current task's time slice. With local
interrupts disabled, `sched_reschedule` updates queue ownership and the
TSS stack pointer. It saves FS and floating-point state, then
`sched_context_switch` changes the saved stack pointer and CR3.
The newly executing context completes the outgoing switch. It can then
reclaim a zombie's old kernel stack. The work-stealing path only takes
an unpinned ready thread that is off its old CPU.

The population test holds 512 workers behind a barrier, records each
CPU's actual runnable switches, and checks a completion deadline for
each worker. The entry-stack test exercises the normal TSS stack and
both IST stacks. [Scheduler](../kernel/sched/sched.c) · [Assembly switch](../kernel/arch/x86_64/switch.S) · [Population test](../scripts/test_threads.py) · [Stack test](../scripts/test_entry_stacks.py)

## Copy-on-write fault

Fork makes resident private pages read-only in both processes and takes
a reference for each copied mapping. A write to a COW page enters
`vmm_page_fault_handler`. If the frame has multiple owners, the handler
allocates and copies a private frame before publishing the writable PTE.
A sole owner can reuse its frame. Inherited shared mappings keep the
original frame. Unmapped addresses and invalid permissions terminate
the faulting process. A retired mapping is invalidated before its frame
can be reused. The shared mapping and TLB campaigns check both page
contents and final reclamation. [Fault handler](../kernel/mm/vmm.c) · [Anonymous mappings](../kernel/mm/anonymous.c) · [Shared mapping campaign](../tests/guest/shared_campaign.c) · [TLB campaign](../scripts/test_tlb.py)

## Blocking pipe transfer

`sys_read_impl` takes a reference to the file before entering its pipe
operation. A pipe reader with no data enrolls under the condition lock.
The scheduler drops that lock only after the wait is visible.
A writer that publishes bytes wakes readers; a last-writer close wakes
them to return EOF after buffered data. A cancelled reader unwinds
the wait before releasing the syscall's file reference. The writer
returns `EPIPE` when the last reader closes. Poll also compares the
descriptor generation, so a replacement cannot redirect a sleeping
poll to another object. [Pipe operations](../kernel/ipc/pipe.c) · [Wait queues](../kernel/sched/sched.c) · [IPC campaign](../scripts/test_ipc.py) · [Cancellation campaign](../scripts/test_cancel.py)

## Durable file replacement

An application writes a temporary file, checks `fsync`, and renames it
on the same mount. StrataFS stages affected directory and inode blocks
in one journal transaction. `journal_commit` flushes its descriptor and
payload, flushes the commit record, and publishes the replay range.
Only then does it update the home blocks and checkpoint the journal.
A failed barrier returns an error and stops further mutations until
remount. Before publication a crash keeps the old namespace. After
publication recovery validates and reapplies the transaction. Open
descriptors retain the old inode until the final close.
[Rename and orphan handling](../kernel/fs/stratafs.c) · [Journal](../kernel/fs/journal.c) · [Namespace campaign](../scripts/test_storage_namespace.py) · [Interruption campaign](../scripts/test_wal_crash.py)

## Reliable TCP request

The virtio-net receive interrupt wakes the network worker.
`net_poll` takes a received frame through Ethernet and IPv4 validation
into `tcp_input`. The PCB bounds out-of-order bytes and four
transmit slots. Cumulative acknowledgments retire those slots and
duplicate ACKs or an expired timer can retransmit the oldest
outstanding segment. Application file readiness wakes a native
`ledgerd` worker, which checks the request, appends a checksummed
record and flushes it. The primary waits for the peer's durable
reply before replying to the client. If the peer fails after the local
commit, repeating the same identity and payload is safe.
[Network worker](../kernel/net/net.c) · [TCP](../kernel/net/tcp.c) · [Service](../user/apps/ledgerd.c) · [Record storage](../user/apps/ledger_store.c) · [Wire campaign](../scripts/test_tcp_campaign.py) · [Recovery campaign](../scripts/test_ledger_load.py)
