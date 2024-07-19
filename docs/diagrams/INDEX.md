# Architecture diagrams

The editable Graphviz source and rendered SVG are listed together.

| Diagram | Editable source |
| --- | --- |
| [Stratum subsystem boundaries](rendered/architecture.svg) | [DOT](source/architecture.dot) |
| [Firmware to the first user instruction](rendered/boot.svg) | [DOT](source/boot.dot) |
| [Four-level address spaces](rendered/address-layout.svg) | [DOT](source/address-layout.dot) |
| [Physical pages and COW ownership](rendered/page-ownership.svg) | [DOT](source/page-ownership.dot) |
| [Application processor startup](rendered/ap-startup.svg) | [DOT](source/ap-startup.dot) |
| [Privilege entry and nested interrupt stacks](rendered/trap-stacks.svg) | [DOT](source/trap-stacks.dot) |
| [Scheduler task ownership](rendered/task-states.svg) | [DOT](source/task-states.dot) |
| [Blocking wait, notification and cancellation](rendered/wait-protocol.svg) | [DOT](source/wait-protocol.dot) |
| [Lock dependencies and execution contexts](rendered/lock-context.svg) | [DOT](source/lock-context.dot) |
| [Process, descriptor and open-file lifetime](rendered/descriptor-lifetime.svg) | [DOT](source/descriptor-lifetime.dot) |
| [Split virtqueue buffer ownership](rendered/virtqueue.svg) | [DOT](source/virtqueue.dot) |
| [Mounted filesystems and StrataFS layout](rendered/disk-layout.svg) | [DOT](source/disk-layout.dot) |
| [Journal commit and durable acknowledgment](rendered/journal-commit.svg) | [DOT](source/journal-commit.dot) |
| [Recovery and allowed interruption outcomes](rendered/crash-recovery.svg) | [DOT](source/crash-recovery.dot) |
| [Ethernet receive and bounded TCP state](rendered/packet-socket.svg) | [DOT](source/packet-socket.dot) |
| [Ledger request, replica and reboot recovery](rendered/record-service.svg) | [DOT](source/record-service.dot) |

See the [source walkthroughs](../WALKTHROUGHS.md) for the linked execution paths.
