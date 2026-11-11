#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

/* Offset the cases defined in dietlibc. */
#define ERRNOBASE 256

#define EQUIESCENCE (ERRNOBASE + 0)
#define ECASFAIL (ERRNOBASE + 1)
/*
 * The invocation did not complete because the server component
 * faulted, and is being (or was) reset by its manager (see component
 * recovery in the kernel's recovery.h). The server's state was lost;
 * the invocation can be retried.
 */
#define ECOMPRESET (ERRNOBASE + 2)
