# Native user ABI

Stratum runs static x86-64 ELF executables in Ring 3. The loader requires a
file-backed executable entry point for the kernel image and a file-backed executable
user segment for a user image. User code follows the integer System V ABI.
Floating-point and SIMD instructions trap until the kernel saves that state per thread.

Invoke `syscall` with the call number in `RAX` and arguments in
`RDI, RSI, RDX, R10, R8, R9`. The result is in `RAX`. Negative values are
Stratum errno values. `RCX` and `R11` are clobbered. Use
[strat_api.h](../../include/user/strat_api.h) for native wrappers and
[syscall_nums.h](../../include/shared/syscall_nums.h) for the number registry.

The dispatcher supports exit, fork, execve, waitpid, getpid, file descriptors and
pipes, poll, anonymous mapping and protection, basic filesystem operations, sockets, uptime, sleep, trace events,
and sysinfo. Reserved calls return `ENOSYS`. Ring 3 faults exit with
`128 + vector`. If `waitpid` cannot copy the exit status to the caller,
it returns `EFAULT` and the child remains available to reap.

Open a directory with `O_RDONLY | O_DIRECTORY`. `readdir` returns one entry,
zero at end of directory, or a negative error. A failed user copy leaves
the directory cursor unchanged.

The kernel starts the embedded native init binary and mounts the boot
archive read-only at `/bin`. The shell looks up bare executable names in
`/bin`, accepts absolute paths, and supports quoting, pipes, input
redirection, output truncation, and append redirection.

`execve(path, argv, envp)` accepts a regular executable file up to 4 MiB.
Each vector can hold up to 32 entries plus a null terminator. The combined
argument and environment strings can occupy up to 4,096 bytes including
terminators. A null vector is empty. The initial stack contains `argc`,
`argv`, `envp`, and an `AT_NULL` entry. A successful exec preserves the
process ID, closes descriptors opened with `O_CLOEXEC`, and starts the new
image. Failure returns a negative errno to the old image without closing
its descriptors. Native user threads, TLS setup, and file-backed mapping
are pending.
