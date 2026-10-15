/*
 * zw_redact.h - credentials kept out of what the panel records about its Z-Wave link
 * (docs/features/36 ZW-030): the frame trace (the developer link, `zwave trace` on the log),
 * diagnostics and fixtures.
 *
 * Security 2 traffic is ciphertext on the wire and stays so. What can carry a PIN or a code in the
 * clear is an application command sent or received without encryption - an Entry Control
 * Notification from a keypad that fell back to no security, a User Code or User Credential
 * report, a door lock's log record - also inside the clear encapsulations (Multi Channel,
 * Supervision, CRC-16, Multi Command) and in Transport Service segments, which can split one.
 * These blank such a command's data after its command byte, so the trace still shows which
 * command went by, and make the frame's checksum right again.
 *
 * A frame that does not parse whole (damaged, cut off by the byte timeout) is not trusted to say
 * where its command is: everything after its FUNC byte is blanked.
 *
 * Portable C11, no state; in place. The caller redacts a copy of what it keeps, never the frame
 * the protocol still needs.
 */
#ifndef ZW_REDACT_H
#define ZW_REDACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether a command class carries credentials in its data: Entry Control (6F), User Code (63),
 * User Credential (83), Door Lock Logging (4C), Security 0 (98: its network key set is clear
 * under a fixed key), Transport Service (55: segments of any of them). */
bool zw_redact_sensitive_cc(uint8_t cc);

/* A clear application command, walked through its clear encapsulations: a sensitive one's bytes
 * after its command byte set to 00. True when anything was blanked. */
bool zw_redact_command(uint8_t *cmd, size_t len);

/* A Serial API data frame as the wire carries it (SOF LEN TYPE FUNC PAYLOAD CHK): the commands in
 * ApplicationCommandHandler (04), BridgeApplicationCommandHandler (A8) and SendData (13) redacted
 * with either node-ID width, the checksum made again; a frame that doesn't parse blanked after its
 * FUNC byte. ACK/NAK/CAN and text are left alone. True when anything changed. */
bool zw_redact_frame(uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_REDACT_H */
