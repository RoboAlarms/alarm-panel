"""Vectors for test_plink_session.c, computed independently of the C code with Python's
`cryptography` (OpenSSL's X25519, HKDF and AES-CCM) and the standard library (SHA-256, HMAC),
straight from docs/features/11 "Sessions" and "Frame format (v1)" (LINK-003, LINK-015):

    K_hs   = HKDF-Expand(K_pair, "plink v1 hello", 32)
    HELLO  = header || proto, caps, capacities section, n_k, e_k || mac1 = HMAC(K_hs, before)[0..15]
    TH_s   = SHA-256(HELLO frame || WELCOME frame up to mac2)
    PRK_s  = HKDF-Extract(n_k || n_m, K_pair || X25519(e, E_peer))
    HKDF-Expand(PRK_s, "plink v1 session" || TH_s, 68) = K_m2k || K_k2m || K_fin || sid (LE)
    WELCOME = header (sid 0) || proto, caps, n_m, e_m || mac2 = HMAC(K_fin, before)[0..15]
    sealed frame = header || AES-128-CCM(payload, nonce = dir || sid || sender || net_id || seq,
                   associated data = the header) || 8-byte tag

The random numbers are scripted: the C test feeds the same bytes through its port, in the order
plink_session.c draws them (the ephemeral private key 32, then the nonce 16, on each side). Run
it from anywhere:

    python components/core/panel_link/test/vectors_plink_session.py
"""

import hashlib
import hmac
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def pub(priv):
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw)


def x25519(priv, peer):
    return X25519PrivateKey.from_private_bytes(priv).exchange(
        X25519PublicKey.from_public_bytes(peer))


def mac16(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()[:16]


def hdr(type_, flags, net_id, sender, sid=0, seq=0):
    return b"AL" + bytes([1, type_, flags]) + struct.pack("<HHII", net_id, sender, sid, seq)


def c_bytes(name, b):
    lines = []
    for i in range(0, len(b), 16):
        lines.append("    " + ", ".join("0x%02x" % x for x in b[i:i + 16]) + ",")
    print(f"/* {name} ({len(b)} bytes) */")
    print("\n".join(lines))


K_PAIR = bytes(range(0xC0, 0xE0))
KP_STREAM = bytes((0x2D * i + 0x61) & 0xFF for i in range(48))  # e_k 32, n_k 16
MS_STREAM = bytes((0x6B * i + 0x05) & 0xFF for i in range(48))  # e_m 32, n_m 16
NET, DEVICE = 0x5A17, 3
e_k, n_k = KP_STREAM[:32], KP_STREAM[32:]
e_m, n_m = MS_STREAM[:32], MS_STREAM[32:]

k_hs = HKDFExpand(hashes.SHA256(), 32, b"plink v1 hello").derive(K_PAIR)
caps = bytes([5]) + struct.pack("<BHBB", 4, 128, 32, 16)
body = hdr(0x10, 0x00, NET, DEVICE) + struct.pack("<BH", 1, 0) + caps + n_k + pub(e_k)
hello = body + mac16(k_hs, body)

wbody = hdr(0x11, 0x02, NET, 0) + struct.pack("<BH", 1, 0) + n_m + pub(e_m)
dh = x25519(e_m, pub(e_k))
assert dh == x25519(e_k, pub(e_m))
th = hashlib.sha256(hello + wbody).digest()
prk = hmac.new(n_k + n_m, K_PAIR + dh, hashlib.sha256).digest()
okm = HKDFExpand(hashes.SHA256(), 68, b"plink v1 session" + th).derive(prk)
k_m2k, k_k2m, k_fin = okm[0:16], okm[16:32], okm[32:64]
sid = struct.unpack("<I", okm[64:68])[0] or 1
welcome = wbody + mac16(k_fin, wbody)

print(f"sid: 0x{sid:08x}")
c_bytes("HELLO", hello)
c_bytes("WELCOME", welcome)
c_bytes("K_m2k", k_m2k)
c_bytes("K_k2m", k_k2m)


def seal(key, type_, flags, sender, seq, payload):
    h = hdr(type_, flags, NET, sender, sid, seq)
    nonce = bytes([0x4D if flags & 0x02 else 0x4B]) + struct.pack("<IHHI", sid, sender, NET, seq)
    return h + AESCCM(key, tag_length=8).encrypt(nonce, payload, h)


# The keypad's first frame (a HEARTBEAT, seq 1) and the master's (a TIME, seq 1).
c_bytes("keypad seq 1", seal(k_k2m, 0x2A, 0x01, DEVICE, 1, bytes(range(0x90, 0x99))))
c_bytes("master seq 1", seal(k_m2k, 0x2B, 0x03, 0, 1, bytes(range(0xA0, 0xAC))))
