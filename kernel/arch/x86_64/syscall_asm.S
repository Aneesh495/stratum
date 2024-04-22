/* Stratum OS System Call Entry and Usercopy Assembly */
.section .text
.global syscall_entry
.global user_enter
.global copy_from_user
.global copy_to_user
.global __usercopy_start
.global __usercopy_end
.global __usercopy_fault
.extern syscall_dispatch

/*
 * syscall_entry:
 * Invoked by x86-64 'syscall' instruction from Ring 3.
 * Hardware state upon entry:
 *   %rcx: user RIP
 *   %r11: user RFLAGS
 *   %rax: syscall number
 *   %rdi, %rsi, %rdx, %r10, %r8, %r9: arguments
 *   %rsp: user stack pointer
 *   Interrupts disabled by MSR_SFMASK
 */
syscall_entry:
    swapgs                      /* Switch to kernel per-CPU GS (points to cpu_t) */
    movq %rsp, %gs:8            /* Save user %rsp into cpu->user_rsp */
    movq %gs:0, %rsp            /* Load kernel stack top from cpu->kernel_stack */

    /* Push user context onto kernel stack (matches syscall_regs_t) */
    pushq $0x1B                 /* SS: USER_DS */
    pushq %gs:8                 /* RSP: user_rsp */
    pushq %r11                  /* RFLAGS: user_rflags */
    pushq $0x23                 /* CS: USER_CS */
    pushq %rcx                  /* RIP: user_rip */

    /* Push General Purpose Registers */
    pushq %rax                  /* Syscall number */
    pushq %rdi                  /* Arg 1 */
    pushq %rsi                  /* Arg 2 */
    pushq %rdx                  /* Arg 3 */
    pushq %r10                  /* Arg 4 */
    pushq %r8                   /* Arg 5 */
    pushq %r9                   /* Arg 6 */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* First argument to syscall_dispatch is pointer to syscall_regs_t */
    movq %rsp, %rdi
    callq syscall_dispatch

    /* Store return value into saved rax slot on stack (offset 96) */
    movq %rax, 96(%rsp)

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax                   /* Pops return value into %rax */

    /* Restore user RIP and RFLAGS for sysret */
    popq %rcx                   /* User RIP */
    addq $8, %rsp               /* Skip saved CS */
    popq %r11                   /* User RFLAGS */
    popq %rsp                   /* Restore user RSP */
    /* Saved SS is left on abandoned kernel stack */

    swapgs                      /* Restore user GS */
    sysretq                     /* Fast return to Ring 3 */

/*
 * user_enter(uint64_t entry_rip, uint64_t user_rsp)
 * Transitions from kernel Ring 0 to user Ring 3 using iretq.
 */
user_enter:
    cli

    /* Reset data segments to USER_DS (0x1B) */
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    /* Build iretq stack frame */
    pushq $0x1B                 /* SS: USER_DS */
    pushq %rsi                  /* RSP: user_rsp */
    pushq $0x202                /* RFLAGS: IF enabled */
    pushq $0x23                 /* CS: USER_CS */
    pushq %rdi                  /* RIP: entry_rip */

    swapgs                      /* In userland, GS_BASE is user, KERNEL_GS_BASE is cpu_t* */
    iretq                       /* Enter Ring 3! */

/*
 * user_enter_regs(const syscall_regs_t *regs)
 * Restores user register frame from pointer in %rdi and returns to Ring 3.
 */
.global user_enter_regs
user_enter_regs:
    cli
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    movq %rdi, %rsp             /* Set stack pointer to regs structure */

    /* Pop general purpose registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax

    /* Rest of frame is: rip, cs, rflags, rsp, ss */
    swapgs
    iretq

/*
 * Safe usercopy primitives with exception fixup table
 */
__usercopy_start:

copy_from_user:
    testq %rdx, %rdx
    jz .Lcfu_done
    movabsq $0x0000800000000000, %rax
    cmpq %rax, %rsi
    jae __usercopy_fault
    subq %rsi, %rax
    cmpq %rax, %rdx
    ja __usercopy_fault
    movq %rdx, %rcx
.Lcfu_loop:
    movb (%rsi), %al
    movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decq %rcx
    jnz .Lcfu_loop
.Lcfu_done:
    xorq %rax, %rax
    ret

copy_to_user:
    testq %rdx, %rdx
    jz .Lctu_done
    movabsq $0x0000800000000000, %rax
    cmpq %rax, %rdi
    jae __usercopy_fault
    subq %rdi, %rax
    cmpq %rax, %rdx
    ja __usercopy_fault
    movq %rdx, %rcx
.Lctu_loop:
    movb (%rsi), %al
    movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decq %rcx
    jnz .Lctu_loop
.Lctu_done:
    xorq %rax, %rax
    ret

__usercopy_end:

__usercopy_fault:
    movq $-14, %rax             /* -STRATUM_EFAULT */
    ret
