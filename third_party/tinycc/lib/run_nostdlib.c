/* ------------------------------------------------------------- */
/* support for run_nostdlib() */

// FIXME: implement arm64 assembler
#if defined(__aarch64__)
#define	USE_ARM64_ASM
static void *alloca_arm64(unsigned long);
__asm__(
#ifdef __leading_underscore
    "_alloca_arm64:\n\t"
#else
    "alloca_arm64:\n\t"
#endif
    ".int 0x91003c00\n\t"	// add x0, x0, #15
    ".int 0x927cec00\n\t"	// and x0, x0, #-16
    ".int 0xcb2063ff\n\t"	// sub sp, sp, x0
    ".int 0x910003e0\n\t"	// mov x0, sp
    ".int 0xd65f03c0"		// ret
);
static void goto_arm64(void *start);
__asm__(
#ifdef __leading_underscore
    "_goto_arm64:\n\t"
#else
    "goto_arm64:\n\t"
#endif
    ".int 0xd61f0000"		// br x0
);
#endif

void _run_nostdlib(void *start, int argc, char **argv, char **envp)
{
#if !defined(_WIN32)
    int i, n = 1;
    unsigned long l;
    char **sp, **e = envp;

    if (envp)
	while (*e++)
           n++;
    l = (((unsigned long) argc + n + 2 + 1) & -2) * sizeof(char *);
    /* nostdlib so avoid alloca() */
    /* also code below will be removed because compiler detects dead store */
#if defined(USE_ARM64_ASM)
    sp = alloca_arm64(l);
#else
#if defined(__aarch64__)
    __asm__("sub sp, sp, %1\n"
            "\tmov %0, sp"
#elif defined(__arm__)
    __asm__("sub sp, sp, %1\n"
            "\tmov %0, sp"
#elif defined(__i386__)
    __asm("sub %1, %%esp\n"
           "\tmov %%esp, %0"
#elif defined(__riscv)
    __asm__("sub sp, sp, %1\n"
            "\tmv %0, sp" 
#elif defined(__x86_64__)
    __asm__("subq %1, %%rsp\n"
            "\tmovq %%rsp, %0"
#endif
            : "=r" (sp)
            : "r" (l));
#endif
    /* create sysv memory layout: argc, argv[], NULL, envp[], NULL */
    sp[0] = (char *) (__SIZE_TYPE__) argc;
    for (i = 0; i < argc; i++)
	sp[i + 1] = argv[i];
    sp[argc + 1] = (char *) 0;
    if (envp)
        for (i = 0; i < n; i++)
	    sp[i + argc + 2] = envp[i];
    else
        sp[argc + 2] = (char *) 0;
#endif

    /* goto *start does not work for clang. Use assembly. */
#if defined(USE_ARM64_ASM)
    goto_arm64(start);
#else
#if defined(__aarch64__)
    __asm__("br %0" : : "r" (start));
#elif defined(__arm__)
    __asm__("mov pc, %0" : : "r" (start));
#elif defined(__i386__)
    __asm__("jmp *%0" : : "r" (start));
#elif defined(__riscv)
    __asm__("jalr %0" : : "r" (start));
#elif defined(__x86_64__)
    __asm__("jmp *%0" : : "r" (start));
#endif
#endif
}
