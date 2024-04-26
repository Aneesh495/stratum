# Virtual memory layout

Stratum uses x86-64 four-level paging. The kernel is linked at
`0xffffffff80000000` and maps the first 4 GiB of physical space at
`0xffff800000000000`. Early boot and AP startup retain an identity mapping
in the kernel page table.

User page tables share supervisor-only higher-half mappings. User ELF segments
are private, page-aligned mappings with permissions derived from the ELF flags.
Writable-executable and overlapping pages are rejected. The 64 KiB user stack
ends at `0x00007ffffffff000` with an unmapped page below it.

`fork` retains shared user pages and clears write permission on writable
mappings. A write fault either copies the page or restores write permission
when only one reference remains. An unhandled Ring 3 fault terminates the
process. Processes with workers keep their address space on one CPU. Anonymous mappings are
demand-zero, non-executable, and limited to 16 MiB per region. Protection and
unmap operations apply to complete regions. Shared mappings, file-backed
mappings, and guarded kernel stacks remain open.

The direct and identity aliases of kernel text and read-only data are
read-only and non-executable. The higher-half text mapping remains
executable. The low AP bootstrap mapping is a separate startup exception.

Shared kernel mappings can be replaced using a synchronous invalidation.
The initiator publishes an address and a generation to each online CPU.
Each CPU acknowledges that generation after invalidation. A competing
initiator receives `EBUSY`. A timed-out invalidation disables further
requests. The caller must retain the retired page until the invalidation
completes. The guest campaign primes the old translation on every CPU,
replaces it, and checks writes to the new frame and the contents of the
retired frame before recycling it.
