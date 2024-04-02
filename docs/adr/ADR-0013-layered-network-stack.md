# ADR-0013: Layered Network Stack and BSD Socket API

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
Inter-node cluster replication requires standard network protocols (Ethernet, ARP, IPv4, ICMP, UDP, and reliable stream TCP) without relying on external host network stacks.

## Decision
We author an original freestanding network stack in `kernel/net/`:
- Ethernet II framing with dynamic ARP cache and resolution.
- IPv4 packet forwarding, TTL decrementing, and 16-bit Internet checksum calculation.
- ICMP echo responder for ping diagnostics.
- UDP datagram endpoints.
- Full TCP state machine: 3-way handshake (SYN, SYN-ACK, ACK), sliding window sequence numbering, reliable payload transmission, retransmissions, and 4-way teardown (FIN, ACK).
- BSD Socket API (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`) mapped to process file descriptors.

## Consequences
- Native peer-to-peer TCP communication between separate QEMU VM guests.
- Zero external library dependencies.
