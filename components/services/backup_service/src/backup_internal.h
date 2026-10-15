/*
 * backup_internal.h - between backup_service.c and its PSA crypto port.
 */
#ifndef BACKUP_INTERNAL_H
#define BACKUP_INTERNAL_H

#include <stdbool.h>

#include "backup/backup.h"

const backup_crypto_t *backup_psa_port(void);

/* RFC 7914's PBKDF2 vector and the GCM specification's test case 16, through the port. */
bool backup_psa_known_answers(void);

#endif /* BACKUP_INTERNAL_H */
