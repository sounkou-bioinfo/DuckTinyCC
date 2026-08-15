extern char etext[], _etext[];
extern char edata[], _edata[];
extern char end[], _end[];

int check_linker_symbols(void)
{
    int errors = 0;

    errors += etext != _etext;
    errors += edata != _edata;
    errors += end != _end;
    return errors;
}
