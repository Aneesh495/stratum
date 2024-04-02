# Native user ABI

Stratum runs static x86-64 ELF executables in Ring 3. The loader requires a
file-backed executable entry point for the kernel image and an executable
user segment for a user image. User code follows the integer System V ABI.
Floating-point and SIMD instructions trap until the kernel saves that state per thread.

Invoke `syscall` with the call number in `RAX` and arguments in
`RDI, RSI, RDX, R10, R8, R9`. The result is in `RAX`. Negative values are
Stratum errno values. `RCX` and `R11` are clobbered. Use
[strat_api.h](../../include/user/strat_api.h) for native wrappers and
[syscall_nums.h](../../include/shared/syscall_nums.h) for the number registry.

The dispatcher supports exit, fork, waitpid, getpid, file descriptors and
pipes, poll, basic filesystem operations, sockets, uptime, sleep, trace events,
and sysinfo. Reserved calls return `ENOSYS`. Ring 3 faults exit with
`128 + vector`. If `waitpid` cannot copy the exit status to the caller,
it returns `EFAULT` and the child remains available to reap.

The kernel starts the embedded native init binary. General exec, native user
threads, TLS setup, and memory-mapping syscalls are pending.
