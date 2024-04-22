#include <shared/syscall_nums.h>

.section .text
.global strat_thread_start
.type strat_thread_start, @function
strat_thread_start:
    xorq %rbp, %rbp
    movq %rdi, %rax
    movq %rsi, %rdi
    callq *%rax
    movl %eax, %edi
    movl $SYS_thread_exit, %eax
    syscall
    ud2
.size strat_thread_start, . - strat_thread_start
