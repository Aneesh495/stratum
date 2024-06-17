# IPv4 and record service contract

An MSI-X receive interrupt wakes a dedicated kernel worker on the bootstrap CPU.
The worker also runs the TCP timers and falls back to timed polling when an
interrupt is unavailable.
Its default static address is `10.0.2.15/24`, with gateway `10.0.2.2`. An
`ip=<address>` line in the EFI partition's `cmdline.txt` sets the guest
address for isolated Ethernet links.

The stack implements Ethernet II, ARP, local IPv4 demultiplexing, ICMP echo
replies, bounded UDP receive queues, and a small TCP stream implementation.
IPv4 fragments are rejected. TCP tracks up to four transmitted segments and
reassembles reordered bytes within its 16 KiB receive window. Cumulative ACKs
retire transmit slots. Three duplicate ACKs trigger fast retransmission. The
bounded send window grows on acknowledged segments, shrinks on fast
retransmit, and drops to one segment after a timeout. The timeout starts at
250 ms and backs off through six retries. DHCP, IPv6, SACK, adaptive RTT
estimation, and comprehensive protocol conformance are outside this profile.

Native sockets are nonblocking and use negative Stratum errno values.
`poll` reports readiness. TCP `connect` can return `EAGAIN` while the
handshake is pending. `shutdown(SHUT_WR)` can return `EAGAIN` while an
transmit window has no room for FIN.

## Record protocol

`ledgerd` is a foreground native user program on TCP port 9090. Each
legacy connection carries one ASCII request and one response, each ending in LF.
A client can send `SESSION 1` first to receive `OK session 1` and exchange
multiple ordered requests and responses on the same connection.

| Request | Response |
| --- | --- |
| `APPEND id value` | `OK durable` after flush, including an identical retry |
| `GET id` | `OK value` or `ERR missing` |
| Conflicting append | `ERR conflict` |
| Invalid input or storage failure | `ERR syntax`, `ERR storage`, or `ERR full` |
| Replica unavailable | `ERR replica`, retry the same append |
| Client limit reached | `ERR busy` |

IDs contain 1 to 24 lowercase ASCII letters, digits, or hyphens. Values contain
1 to 128 printable ASCII bytes. The service has 32 workers. The listener passes
accepted descriptors through a native pipe. A worker reads requests with bounded
deadlines and serializes storage operations through a futex.

New records occupy 160-byte checksummed entries in `/ledger/.records-v1`. Each
entry has a little-endian 32-bit FNV-1a checksum, a zero-padded 25-byte identity,
a zero-padded 129-byte value, and two zero bytes reserved for the format. The
checksum covers every byte after the checksum field. Each complete entry is one
StrataFS write and must pass `fsync` before the service updates the index or
acknowledges it. Restart scans the log and rebuilds the index, which supports up
to 16,384 distinct records. An invalid or incomplete record prevents startup.
The service also reads records created by older versions and by `ledgerput` in
`/ledger/<id>`.

Use `ledgerd <peer-ip>` on the primary and `ledgerd` on the replica. The primary
forwards each successful append or identical retry to the peer and waits for a
durable reply before acknowledging the client. If the peer fails after the local
commit, the client can retry the same identity and payload. Replication uses one
authority with a synchronous peer and no election. `ledgerpull` still performs a
single-record fetch and local commit.

The host-client smoke test uses QEMU user networking. The two-guest test
forwards Ethernet frames over an isolated QEMU socket link and independently
checks the request and response bytes in a packet capture. The relay drops the
first application reply, and the capture must show the retransmitted segment.


`make test-tcp-reference` boots a pinned Alpine Linux guest on the isolated
Ethernet link and reconstructs 32 exact streams from the capture. Set
`STRATUM_REFERENCE_ISO` to the absolute path to
`alpine-virt-3.22.8-x86_64.iso`. The runner verifies the image checksum before
using it. It needs Zig for a static Linux test client and a tar implementation
that reads ISO9660, such as bsdtar. [Alpine release archive](https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/)
