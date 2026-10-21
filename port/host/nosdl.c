#include <stdio.h>

#include "host.h"

int host_sdl_active(void) { return 0; }

int host_sdl_run(void)
{
    fputs("p4dos: built without SDL2; use 'make sdl' and run build/p4dos-sdl\n", stderr);
    return 2;
}
