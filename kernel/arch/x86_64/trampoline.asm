; kernel/arch/x86_64/trampoline.asm - SMP Application Processor Startup Trampoline
; Assembled with: nasm -f bin trampoline.asm -o trampoline.bin

[BITS 16]
[ORG 0x8000]

trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary 32-bit GDTR
    lgdt [0x8000 + (gdtr_desc - trampoline_start)]

    ; Enable Protected Mode (CR0.PE = 1)
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to 32-bit code (Segment 0x08)
    jmp dword 0x08:(0x8000 + (ap_prot32 - trampoline_start))

[BITS 32]
ap_prot32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable PAE (bit 5) and PGE (bit 7) in CR4
    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax

    ; Load CR3 from mailbox (0x8F00)
    mov eax, dword [0x8F00]
    mov cr3, eax

    ; Enable Long Mode and NX in EFER MSR (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ; Enable Paging (PG=31, WP=16, PE=0) in CR0
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16) | 1
    mov cr0, eax

    ; Far jump to 64-bit Long Mode (Segment 0x18)
    jmp dword 0x18:(0x8000 + (ap_long64 - trampoline_start))

[BITS 64]
DEFAULT ABS
ap_long64:
    ; Reset data segment registers in 64-bit mode
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Load AP stack pointer from mailbox (0x8F08)
    mov rsp, [0x8F08]

    ; Load CPU ID into RDI (SysV ABI first argument) from mailbox (0x8F18)
    mov edi, dword [0x8F18]

    ; Jump to 64-bit C entry point from mailbox (0x8F10)
    mov rax, [0x8F10]
    call rax

ap_halt:
    cli
    hlt
    jmp ap_halt

align 16
gdt_start:
    ; 0x00: Null descriptor
    dq 0x0000000000000000
    ; 0x08: 32-bit Code Segment (Base=0, Limit=4GB, Access=0x9A, Flags=0xCF)
    dq 0x00CF9A000000FFFF
    ; 0x10: 32-bit Data Segment (Base=0, Limit=4GB, Access=0x92, Flags=0xCF)
    dq 0x00CF92000000FFFF
    ; 0x18: 64-bit Code Segment (Base=0, Limit=0, Access=0x9A, Flags=0xAF)
    dq 0x00AF9A000000FFFF
gdt_end:

align 4
gdtr_desc:
    dw (gdt_end - gdt_start - 1)
    dd (0x8000 + (gdt_start - trampoline_start))

trampoline_end:
