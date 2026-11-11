#ifndef IO_H
#define IO_H
#include <cos_types.h>
/* outb/inb come from the kernel's shared cos_io.h */
#include <cos_io.h>

static inline void
outl(u16_t port, u32_t value)
{
    __asm__ __volatile__("outl %1, %0"
        : 
        : "dN" (port), "a" (value));
}

static inline u32_t
inl(u16_t port)
{
    u32_t ret;

    __asm__ __volatile__("inl %1, %0"
        : "=a" (ret)
        : "dN" (port));

    return ret;
}

static inline void
outw(u16_t port, u16_t value)
{
    __asm__ __volatile__("outw %1, %0"
        :
        : "dN" (port), "a" (value));
}

static inline u16_t
inw(u16_t port)
{
    u16_t ret;

    __asm__ __volatile__("inw %1, %0"
        : "=a" (ret)
        : "dN" (port));

    return ret;
}

#endif /* IO_H */