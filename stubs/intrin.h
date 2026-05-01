#ifndef _INTRIN_H
#define _INTRIN_H
// MinGW intrin.h stub - use gcc builtins instead
#define __debugbreak() __asm__ __volatile__("int $3")
#define _ReturnAddress() __builtin_return_address(0)
#define _AddressOfReturnAddress() (__builtin_frame_address(0))
static inline void __cpuid(int info[4], int id) {
    __asm__ __volatile__("cpuid" : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3]) : "a"(id));
}
static inline unsigned long long __rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
#endif
