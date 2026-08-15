#include <dlfcn.h>
#include <stdio.h>

typedef int (*add_fn)(int, int);

int main(void)
{
    void *handle;
    add_fn add;

    handle = dlopen("/add.wasm", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    add = (add_fn)dlsym(handle, "add");
    if (!add)
        add = (add_fn)dlsym(handle, "_add");
    if (!add) {
        fprintf(stderr, "dlsym(add) failed: %s\n", dlerror());
        return 2;
    }

    if (add(2, 40) != 42) {
        fprintf(stderr, "add(2, 40) failed\n");
        return 3;
    }

    dlclose(handle);
    puts("ok emscripten dlopen add");
    return 0;
}
