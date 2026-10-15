/* i18n_internal.h - shared by the component's sources (private). */
#ifndef I18N_INTERNAL_H
#define I18N_INTERNAL_H

#include <stddef.h>

/* Length (1..4) of the valid UTF-8 sequence at s, reading at most `avail` bytes; 0 when the bytes
 * there are not a complete, strictly valid sequence. */
size_t i18n_utf8_seq(const unsigned char *s, size_t avail);

#endif
