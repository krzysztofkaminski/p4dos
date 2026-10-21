/* Optional SDL2 front end of the host port. Without SDL these are stubs. */
#ifndef P4DOS_HOST_H
#define P4DOS_HOST_H

#include <stdint.h>

int host_sdl_active(void);
int host_sdl_run(void);

#endif
