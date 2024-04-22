#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#define EXEC_MAX_IMAGE (4U * 1024 * 1024)
#define EXEC_MAX_VECTOR 32
#define EXEC_STRINGS_SIZE 4096
#define EXEC_MAX_STACK (EXEC_STRINGS_SIZE + 2 * (EXEC_MAX_VECTOR + 1) * sizeof(uint64_t) + 32)

typedef struct {
    unsigned argc, envc;
    size_t used;
    size_t args[EXEC_MAX_VECTOR], env[EXEC_MAX_VECTOR];
    char strings[EXEC_STRINGS_SIZE];
} exec_args_t;

static int copy_string(char *dest, const char *source, size_t max, size_t *length) {
    uint64_t address = (uint64_t)source;
    if (!address || address >= USER_SPACE_TOP) return -STRATUM_EFAULT;
    for (size_t i = 0; i < max; i++) {
        if (copy_from_user(dest + i, (const void *)(address + i), 1) != 0) return -STRATUM_EFAULT;
        if (!dest[i]) {
            if (length) *length = i + 1;
            return 0;
        }
    }
    return -STRATUM_E2BIG;
}

static int copy_vector(exec_args_t *args, const char *const *vector, size_t *offsets, unsigned *count) {
    if (!vector) return 0;
    uint64_t address = (uint64_t)vector;
    if (address >= USER_SPACE_TOP) return -STRATUM_EFAULT;
    for (unsigned i = 0; i <= EXEC_MAX_VECTOR; i++) {
        const char *str;
        if (copy_from_user(&str, (const void *)(address + i * sizeof(str)), sizeof(str))) return -STRATUM_EFAULT;
        if (!str) return 0;
        if (i == EXEC_MAX_VECTOR) break;
        size_t length;
        int res = copy_string(args->strings + args->used, str, EXEC_STRINGS_SIZE - args->used, &length);
        if (res) return res;
        offsets[i] = args->used;
        args->used += length;
        *count = i + 1;
    }
    return -STRATUM_E2BIG;
}

static int stage_stack(user_program_t *prog, const exec_args_t *args) {
    uint64_t words[2 * EXEC_MAX_VECTOR + 5];
    uint64_t strings = USER_STACK_TOP - args->used;
    size_t count = 0;
    words[count++] = args->argc;
    for (unsigned i = 0; i < args->argc; i++) words[count++] = strings + args->args[i];
    words[count++] = 0;
    for (unsigned i = 0; i < args->envc; i++) words[count++] = strings + args->env[i];
    words[count++] = 0;
    words[count++] = 0; /* AT_NULL */
    words[count++] = 0;
    uint64_t start = round_down(strings - count * sizeof(uint64_t), 16);
    uint8_t *stack = kzalloc(EXEC_MAX_STACK);
    if (!stack) return -STRATUM_ENOMEM;
    memcpy(stack, words, count * sizeof(uint64_t));
    memcpy(stack + strings - start, args->strings, args->used);
    for (uint64_t va = start; va < USER_STACK_TOP;) {
        uint64_t phys;
        if (vmm_get_mapping(prog->address_space, round_down(va, PAGE_SIZE), &phys, NULL)) {
            kfree(stack);
            return -STRATUM_EFAULT;
        }
        size_t offset = va % PAGE_SIZE;
        memcpy((uint8_t *)phys_to_virt(phys) + offset, stack + (va - start), PAGE_SIZE - offset);
        va += PAGE_SIZE - offset;
    }
    kfree(stack);
    prog->user_stack_top = start;
    return 0;
}

static int load_path(const char *path, user_program_t *prog) {
    file_t *file;
    int res = vfs_open(path, O_RDONLY, 0, &file);
    if (res) return res == -STRATUM_EISDIR ? -STRATUM_ENOEXEC : res;

    vfs_stat_t st;
    res = vfs_fstat(file, &st);
    if (res == 0) {
        if ((st.st_mode & VFS_MODE_IFMT) != VFS_MODE_IFREG || !st.st_size || st.st_size > EXEC_MAX_IMAGE) {
            res = -STRATUM_ENOEXEC;
        } else if (!(st.st_mode & 0111)) {
            res = -STRATUM_EACCES;
        }
    }
    if (res) {
        vfs_close(file);
        return res;
    }
    uint8_t *image = kmalloc(st.st_size);
    if (!image) {
        vfs_close(file);
        return -STRATUM_ENOMEM;
    }
    size_t used = 0;
    while (used < st.st_size) {
        int64_t n = vfs_read(file, image + used, st.st_size - used);
        if (n <= 0) {
            res = n ? (int)n : -STRATUM_ENOEXEC;
            break;
        }
        used += (size_t)n;
    }
    vfs_close(file);
    if (res == 0) res = user_elf_load(image, used, prog);
    kfree(image);
    return res;
}

int process_exec(const char *path, const char *const *argv, const char *const *envp) {
    process_t *proc = process_get_current();
    if (!proc) return -STRATUM_ESRCH;
    if (!process_can_replace_image(proc)) return -STRATUM_EBUSY;
    char kernel_path[VFS_MAX_PATH];
    int res = copy_string(kernel_path, path, sizeof(kernel_path), NULL);
    if (res) return res == -STRATUM_E2BIG ? -STRATUM_ENAMETOOLONG : res;

    exec_args_t *args = kzalloc(sizeof(*args));
    if (!args) return -STRATUM_ENOMEM;
    res = copy_vector(args, argv, args->args, &args->argc);
    if (res == 0) res = copy_vector(args, envp, args->env, &args->envc);

    user_program_t image = {0};
    if (res == 0) res = load_path(kernel_path, &image);
    if (res == 0) res = stage_stack(&image, args);
    kfree(args);
    if (res != 0) {
        if (image.address_space) vmm_destroy_address_space(image.address_space);
        return res;
    }

    /* All fallible preparation finished. Commit with interrupts disabled. */
    thread_t *thread = g_runqueues[smp_get_cpu_id()].current_thread;
    pml4_t *old = proc->address_space;
    proc->address_space = image.address_space;
    thread->address_space = image.address_space;
    memset(proc->regions, 0, sizeof(proc->regions));
    memset(proc->workers, 0, sizeof(proc->workers));
    const char *leaf = strrchr(kernel_path, '/');
    strncpy(proc->name, leaf ? leaf + 1 : kernel_path, sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = 0;
    vmm_switch_address_space(image.address_space);
    thread_reset_user_state(thread);
    vmm_destroy_address_space(old);
    fd_close_exec(&proc->fds);

    syscall_regs_t entry = {
        .rip = image.entry_point, .rsp = image.user_stack_top,
        .cs = 0x23, .ss = 0x1B, .rflags = 0x202,
    };
    user_enter_regs(&entry);
}
