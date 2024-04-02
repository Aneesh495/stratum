# ADR-0011: VFS Abstraction Layer and File Descriptor Tables

Historical design record. See [verification status](../BUILD_STATUS.md) for the current implementation and open work.

## Status
Accepted

## Context
A Unix-like user environment requires polymorphic file operations where regular files, directory nodes, pipes, and network sockets share a unified descriptor table interface.

## Decision
We decouple abstract file operations (`vfs_node_t` and `file_ops_t`) from concrete storage backends:
- VFS provides path resolution (`vfs_lookup`), mounting tables, and reference-counted inodes.
- Process file descriptor tables support `open`, `close`, `read`, `write`, `stat`, `dup2`, and `poll`.
- File descriptions represent open instances and point to underlying VFS nodes, IPC pipe objects, or BSD network sockets.

## Consequences
- Transparent redirection of standard streams (`dup2`) and non-blocking readiness querying (`poll`).
- Clean integration with both in-memory nodes (root, initramfs) and block-backed filesystems (StrataFS).
