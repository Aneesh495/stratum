# IPv4 and record service contract

The normal QEMU guest polls the virtio-net split queues from the bootstrap CPU.
Its default static address is `10.0.2.15/24`, with gateway `10.0.2.2`. An
`ip=<address>` line in the EFI partition's `cmdline.txt` sets the guest
address for isolated Ethernet links.

The stack implements Ethernet II, ARP, local IPv4 demultiplexing, ICMP echo
replies, bounded UDP receive queues, and a small TCP stream implementation.
IPv4 fragments are rejected. TCP uses one outstanding segment per stream,
a receive window, and bounded retransmission. DHCP, IPv6, SACK, congestion
control, and comprehensive TCP interoperability remain open.

Native sockets are nonblocking and use negative Stratum errno values.
`poll` reports readiness. TCP `connect` can return `EAGAIN` while the
handshake is pending. `shutdown(SHUT_WR)` can return `EAGAIN` while an
outstanding segment is awaiting acknowledgment.

## Record protocol

`ledgerd` is a foreground native user program on TCP port 9090. Each
connection carries one ASCII request and one response, each ending in LF.

| Request | Response |
| --- | --- |
| `APPEND id value` | `OK durable` after flush, including an identical retry |
| `GET id` | `OK value` or `ERR missing` |
| Conflicting append | `ERR conflict` |
| Invalid input or storage failure | `ERR syntax` or `ERR storage` |

IDs contain 1 to 24 lowercase ASCII letters, digits, or hyphens. Values contain
1 to 128 printable ASCII bytes. Each record is stored in `/ledger/<id>`.
The service processes one client at a time. `ledgerpull` performs an explicit
single-record fetch and local commit. Authentication, automatic replication,
and consensus remain outside this implementation.

The host-client smoke test uses QEMU user networking. The two-guest test
forwards Ethernet frames over an isolated QEMU socket link and independently
checks the request and response bytes in a packet capture.
