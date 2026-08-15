void __attribute__((section(".text.linker_test"), noinline, used))
linker_test_text(void)
{
}

unsigned char linker_test_data[23]
    __attribute__((section(".data.linker_test"), used)) = { 1 };

unsigned char linker_test_bss[257]
    __attribute__((section(".bss.linker_test"), used));

/* set_global_sym replaces BSS/common definitions of these symbols. */
unsigned char _etext;
unsigned char _edata;
