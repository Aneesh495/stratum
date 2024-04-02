.section .rodata
.global g_user_init_binary
.global g_user_init_binary_len

g_user_init_binary:
    .incbin "build/user_init.elf"
g_user_init_binary_end:

g_user_init_binary_len:
    .quad g_user_init_binary_end - g_user_init_binary

.global g_user_test_binary
.global g_user_test_binary_len
g_user_test_binary:
    .incbin "build/user_test.elf"
g_user_test_binary_end:

g_user_test_binary_len:
    .quad g_user_test_binary_end - g_user_test_binary
