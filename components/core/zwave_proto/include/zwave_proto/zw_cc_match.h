/*
 * zw_cc_match.h - command-classes.md §6: whether an incoming command is the answer to a Get the
 * panel sent. Operates on already-unwrapped command bytes (CC | command | parameters) - §1's
 * encapsulation (endpoint, session ID) is compared by the caller, which already has both
 * commands' unwrap results. Security's own Get/Report exchanges (Nonce, KEX, Commands Supported)
 * are not matched here: their command IDs live in security.md, out of scope for this file.
 */
#ifndef ZW_CC_MATCH_H
#define ZW_CC_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when 'got' is the answer §6's table says belongs to the Get/Get-like command 'sent' -
 * including the parameter check §6 lists (sensor type, notification type, group, endpoint, CC,
 * parameter, indicator ID, Supervision session ID). False for anything §6 does not pair up
 * (including a Set, which §1/§6 say has no command-level answer) or that fails length/parameter
 * checks of its own. */
bool zw_cc_answers(const uint8_t *sent, size_t sent_len, const uint8_t *got, size_t got_len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_MATCH_H */
