/* Hard disk served over the console line by tools/hostdisk.py. See hostdisk.c. */
#ifndef P4DOS_HOSTDISK_H
#define P4DOS_HOSTDISK_H

typedef struct {
    /* Puts one frame on the line in one piece; non-zero if it could not be sent. */
    int (*send)(const void *buf, int len);
    /* Console input that is not a frame; -1 as for kbd_term_char(). */
    void (*key)(int ch);
    /* Sleeps until wake() or for ms. A single-threaded port reads the line here instead
     * and passes it to hostdisk_input(). */
    void (*wait)(int ms);
    void (*wake)(void);
} hostdisk_port_t;

void hostdisk_init(const hostdisk_port_t *port);
/* Every byte from the console line, and -1 when the line goes quiet. */
void hostdisk_input(int ch);
/* True once after a host began to serve: the PC has to reboot for DOS to see the drive. */
int hostdisk_changed(void);
/* At POST: attach or detach the drive. */
void hostdisk_refresh(void);

#endif
