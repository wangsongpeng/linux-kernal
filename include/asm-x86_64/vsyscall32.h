#ifndef _ASM_VSYSCALL32_H
#define _ASM_VSYSCALL32_H 1

/* Values need to match arch/x86_64/ia32/vsyscall.lds */

#define VSYSCALL32_BASE 0xffffe000UL
#define VSYSCALL32_EHDR ((const struct elf32_hdr *) VSYSCALL32_BASE)

#define VSYSCALL32_VSYSCALL ((void *)VSYSCALL32_BASE + 0x400) 
#define VSYSCALL32_SIGRETURN ((void *)VSYSCALL32_BASE + 0x500) 
#define VSYSCALL32_RTSIGRETURN ((void *)VSYSCALL32_BASE + 0x500) 

#endif
