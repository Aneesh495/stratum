# Local release qualification

Execution commit `f0a3dc824a8867e7f29bafe9fb76e434b726db64`.

All 40 fixed commands completed successfully on the reference macOS arm64
TCG host. Independent verification passed A01 through A11. The verifier
rejected all 13 deliberate corruptions of the sealed bundle and verified
the original again afterward. Hosted verification and release publication
completed after this local run. [Hosted run](https://github.com/Aneesh495/stratum/actions/runs/42530758442) · [Release](https://github.com/Aneesh495/stratum/releases/tag/v0.1.0)

| Observation | Verified result |
| --- | --- |
| Boot | 45 fresh boots, 128 loader or handoff rejections, and three nested-stack profiles |
| Storage | 128,006 mixed operations, 100 guest interruptions, and real timeout and flush-error recovery |
| Network | 150,000 exact UDP echoes, 1,000 TCP sessions with 316,211,200 verified payload bytes, and 32 direct Linux reference sessions |
| Service | 32 clients, 100,000 logical requests, 10,000 durable records checked on both disks, and ten server interruptions |
| Soak | 11,320 mixed rounds in 3,600.4 seconds on 8 vCPUs and 1 GiB, with quiescent resources reconciled |
| Production fuzzing | 927,406,765 completed executions across six targets, each exercised for at least 600 seconds |

The baseline comparison used the same host, guest profiles and completed
work for both builds. Four-vCPU SMP work took a median of 2,536,500 guest
TSC ticks on the baseline and 2,522,500 on the candidate. The
interquartile ranges overlap. The raw samples distinguish host
nanoseconds from guest TSC ticks.

Acceptance manifest SHA-256
`f89a211a33cb8510b935f9d376080ae431d224e1f34063fa1723150d85cff030`.

Local release archive SHA-256
`e61475d520d19d877977d6827dd1da09d8b696c37a3ee2687e7c856770f49696`.

The [reproduction runbook](../REPRODUCE.md) lists the underlying
workloads and raw artifact locations.
