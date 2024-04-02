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
process. The current ABI has one runnable thread per address space. Anonymous
demand paging, shared mappings, and guarded kernel stacks remain open.
