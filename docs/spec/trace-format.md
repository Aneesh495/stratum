# Diagnostic trace

Each CPU retains 1,024 records in a ring. The [native trace ABI](../../include/shared/syscall_nums.h) gives each record a raw x86 timestamp counter, CPU ID, event ID, and four 64-bit arguments. Compare order by per-CPU sequence. TSC ticks retain their native units.

`strat_trace_info(cpu, &info)` returns the retained sequence range `[first, next)` and the cumulative count of overwritten records. `strat_trace_read(cpu, sequence, &record)` returns `ERANGE` for an overwritten sequence and `EAGAIN` for an unpublished or concurrently overwritten record. A slot sequence guards publication. Writers disable local interrupts while filling their own CPU's slot. Readers can sample another CPU without taking a writer lock.

| Event | Arguments in order |
| --- | --- |
| 1 | Previous TID, next TID |
| 2 | Syscall number and first three arguments |
| 3 | Faulting virtual address, hardware error code |
| 4 | Block sector, byte count, operation, completion result |
| 5 | Received Ethernet frame length |
| 100 and higher | Native test and user events |

The shell's `trace [cpu]` command prints the last 16 retained sequences. `make test-trace` exercises the native snapshot ABI and verifies the overwritten range and retained payloads.
