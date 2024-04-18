.section .text
.global _start
.extern main
.extern exit

_start:
    /* Clear base pointer for clean stack trace */
    xorq %rbp, %rbp

    /* argc, argv, envp come from the initial process stack. */
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 8(%rsi,%rdi,8), %rdx
    callq main

    /* Call exit(eax) */
    movl %eax, %edi
    callq exit

.Lhalt:
    jmp .Lhalt
