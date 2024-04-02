# Diagnostic trace

The current trace is a kernel memory ring. Each slot stores a raw x86 timestamp
counter, CPU ID, event ID, and four 64-bit arguments. It uses
[trace.h](../../include/kernel/trace.h) as its layout. The timestamp units are
TSC ticks.

The trace currently records syscall dispatch and explicit trace emissions.
The diagnostic boot prints per-CPU head, tail, and dropped-event counts.
A stable serialized trace format and complete event coverage remain open.
