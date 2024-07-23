# Hosted verification and publication

The [full acceptance workflow](../.github/workflows/full-acceptance.yml)
takes the exact execution commit as an input. Publish the execution commit
and the workflow to the default branch before dispatching it. The job
checks that the execution commit is an ancestor of published `main`.
A short CI run covers its own checks. The manually dispatched job also
runs the hour-long soak, the production fuzz targets, the comparison
benchmarks, and the independent evidence controls.

After a successful hosted run, download the full evidence and release
package from that run. Compare the execution SHA, source tree, and
manifest hash with the job result. Verify the archive SHA-256 before
uploading it to a release tied to the execution commit. Verify the
downloadable release asset against the same digest. Keep the execution
commit and workflow publication commit distinct in release notes.

The hosted job pins the reference ISO and Zig archive by SHA-256. Its
environment record identifies the actual runner tools and firmware.
[Zig release index](https://ziglang.org/download/index.json) · [Alpine release archive](https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/)
