/*
 * link_console.h - lines typed at link_sim's console, read on a thread of their own so the main
 * loop keeps serving the network while nobody types (Windows can't wait on a console and a socket
 * together). Works with a pipe too: a script can feed the commands.
 */
#ifndef LINK_CONSOLE_H
#define LINK_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

#define LINK_CONSOLE_LINE 256u

/* Starts the reader; false when no thread could be made. */
bool link_console_start(void);
/* The next whole line, without its line end (false: none waiting). */
bool link_console_line(char *out, size_t cap);
/* The input ended (end of file or a closed pipe) and every line has been taken. */
bool link_console_closed(void);

#endif /* LINK_CONSOLE_H */
