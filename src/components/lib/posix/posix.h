#define SYSCALLS_NUM 500

typedef long (*cos_syscall_t)(long a, long b, long c, long d, long e, long f);

void libc_syscall_override(cos_syscall_t fn, int syscall_num);
/* The handler currently installed for syscall_num (NULL if none), e.g. to chain to it. */
cos_syscall_t libc_syscall_get(int syscall_num);
