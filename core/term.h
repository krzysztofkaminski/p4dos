/* ANSI terminal mirror of the text screen. See term.c. */
#ifndef P4DOS_TERM_H
#define P4DOS_TERM_H

/* Sends bytes to the terminal; returns non-zero if some could not be delivered. */
typedef int (*term_out_fn)(const char *buf, int len);

void term_init(term_out_fn out);
/* Returns non-zero if output was lost; the next call then repaints everything. */
int term_refresh(int force);
void term_close(void);

#endif
