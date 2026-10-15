/* i18n_builtin.h - the built-in catalog table (private). Defined by
 * catalogs/i18n_catalogs_gen.c, which `tools/i18n.py build` generates. */
#ifndef I18N_BUILTIN_H
#define I18N_BUILTIN_H

#include <stddef.h>

#include "i18n/i18n.h"

extern const i18n_catalog_t *const i18n_builtin[];
extern const size_t i18n_builtin_count;

#endif
