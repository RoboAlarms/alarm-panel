"""Vectors for test_plink_pair.c, computed independently of the C code with Python's
`cryptography` (OpenSSL's X25519 and HKDF) and the standard library (SHA-256, HMAC), straight
from docs/features/11 "Pairing" (LINK-004, LINK-016):

    commit = SHA-256("plink v1 commit" || pk_m || pk_k || n_m)
    code   = be32(SHA-256("plink v1 sas" || pk_k || pk_m || n_k || n_m)[0..3]) mod 10^6
    TH     = SHA-256("plink v1 transcript" || u16 len || PAIR_REQ payload || u16 len ||
                     PAIR_COMMIT payload || u16 2 || net_id || u16 16 || n_k || u16 16 || n_m)
    PRK    = HKDF-Extract("plink v1 pair", DH)
    K_pair = HKDF-Expand(PRK, "plink v1 key" || TH, 32)
    K_conf = HKDF-Expand(PRK, "plink v1 conf" || TH, 32)
    mac_k, mac_m = HMAC(K_conf, the frame up to the MAC)[0..15]

It prints the fixed-input vectors and every frame of one honest pairing whose random numbers
are scripted (the C test feeds the same bytes through its port, in the order plink_pair.c draws
them: keypad attempt id 4 + private key 32 at PAIR_REQ, n_k 16 at PAIR_COMMIT; master private
key 32 + n_m 16 at PAIR_REQ). Run it from anywhere:

    python components/core/panel_link/test/vectors_plink_pair.py
"""

import hashlib
import hmac
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.kdf.hkdf import HKDFExpand
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def sha256(*parts):
    return hashlib.sha256(b"".join(parts)).digest()


def pub(priv):
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw)


def x25519(priv, peer):
    return X25519PrivateKey.from_private_bytes(priv).exchange(
        X25519PublicKey.from_public_bytes(peer))


def commitment(pk_m, pk_k, n_m):
    return sha256(b"plink v1 commit", pk_m, pk_k, n_m)


def code(pk_k, pk_m, n_k, n_m):
    h = sha256(b"plink v1 sas", pk_k, pk_m, n_k, n_m)
    return "%06d" % (struct.unpack(">I", h[:4])[0] % 1000000)


def u16(n):
    return struct.pack("<H", n)


def keys(dh, req, commit, net_id, n_k, n_m):
    th = sha256(b"plink v1 transcript", u16(len(req)), req, u16(len(commit)), commit,
                u16(2), u16(net_id), u16(16), n_k, u16(16), n_m)
    prk = hmac.new(b"plink v1 pair", dh, hashlib.sha256).digest()
    k_pair = HKDFExpand(hashes.SHA256(), 32, b"plink v1 key" + th).derive(prk)
    k_conf = HKDFExpand(hashes.SHA256(), 32, b"plink v1 conf" + th).derive(prk)
    return k_pair, k_conf


def mac16(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()[:16]


def c_bytes(name, b):
    """The bytes as a C initializer, 16 a line."""
    lines = []
    for i in range(0, len(b), 16):
        lines.append("    " + ", ".join("0x%02x" % x for x in b[i:i + 16]) + ",")
    print(f"/* {name} ({len(b)} bytes) */")
    print("\n".join(lines))


# ---- fixed inputs -----------------------------------------------------------------------------
PK_K = bytes(range(0x00, 0x20))
PK_M = bytes(range(0x20, 0x40))
N_K = bytes(range(0x40, 0x50))
N_M = bytes(range(0x50, 0x60))
print("commitment:", commitment(PK_M, PK_K, N_M).hex())
print("code:", code(PK_K, PK_M, N_K, N_M))

DH = bytes(range(0x60, 0x80))
REQ = bytes((0xA0 + i) & 0xFF for i in range(50))
COMMIT = bytes((0xB0 + 3 * i) & 0xFF for i in range(60))
kp, kc = keys(DH, REQ, COMMIT, 0x5A17, N_K, N_M)
print("K_pair:", kp.hex())
print("K_conf:", kc.hex())

# A code with leading zeros: the first n_m = i (16 bytes little-endian) whose code is below 1000.
for i in range(1, 1 << 20):
    n_m = i.to_bytes(16, "little")
    c = code(PK_K, PK_M, N_K, n_m)
    if int(c) < 1000:
        print(f"leading zeros: n_m = {i} (16 bytes little-endian) gives code {c}")
        break

# plink_mac_fill: key 70..8f, a 37-byte frame (byte i = 3 i + 1), the MAC over its first 21.
KEY = bytes(range(0x70, 0x90))
FRAME = bytes((3 * i + 1) & 0xFF for i in range(37))
print("mac over frame[0..20]:", mac16(KEY, FRAME[:21]).hex())


# ---- one honest pairing with scripted random numbers ------------------------------------------
def hdr(type_, flags, net_id, sender):
    return b"AL" + bytes([1, type_, flags]) + struct.pack("<HHII", net_id, sender, 0, 0)


def string(text, max_len):
    b = text.encode()[:max_len]
    return bytes([len(b)]) + b


KP_STREAM = bytes((0x31 * i + 7) & 0xFF for i in range(52))   # attempt 4, sk_k 32, n_k 16
MS_STREAM = bytes((0x57 * i + 0x13) & 0xFF for i in range(48))  # sk_m 32, n_m 16
attempt = struct.unpack("<I", KP_STREAM[0:4])[0] or 1
sk_k, n_k = KP_STREAM[4:36], KP_STREAM[36:52]
sk_m, n_m = MS_STREAM[0:32], MS_STREAM[32:48]
pk_k, pk_m = pub(sk_k), pub(sk_m)
NET, DEVICE, HOME = 0x5A17, 3, 1
caps = bytes([5]) + struct.pack("<BHBB", 4, 128, 32, 16)  # partitions, zones, users, devices

req = struct.pack("<IBHB", attempt, 1, 0, 1) + caps + pk_k + string("Hall keypad", 47) + \
    string("1.4.0", 31)
f_req = hdr(0x01, 0x00, 0, 0xFFFF) + req  # the keypad doesn't know the net id yet
commit = commitment(pk_m, pk_k, n_m)
commit_payload = struct.pack("<IBH", attempt, 1, 0) + pk_m + commit + \
    string("Harbor House", 47) + string("a1b2c3", 6)
f_commit = hdr(0x02, 0x02, NET, 0) + commit_payload
f_nonce = hdr(0x03, 0x00, NET, 0xFFFF) + struct.pack("<I", attempt) + n_k
f_reveal = hdr(0x04, 0x02, NET, 0) + struct.pack("<I", attempt) + n_m
dh_k, dh_m = x25519(sk_k, pk_m), x25519(sk_m, pk_k)
assert dh_k == dh_m
k_pair, k_conf = keys(dh_k, req, commit_payload, NET, n_k, n_m)
body = hdr(0x05, 0x00, NET, 0xFFFF) + struct.pack("<I", attempt)
f_confirm = body + mac16(k_conf, body)
body = hdr(0x06, 0x02, NET, 0) + struct.pack("<IBB", attempt, DEVICE, HOME)
f_done = body + mac16(k_conf, body)

print(f"attempt: 0x{attempt:08x}")
print("code:", code(pk_k, pk_m, n_k, n_m))
c_bytes("PAIR_REQ", f_req)
c_bytes("PAIR_COMMIT", f_commit)
c_bytes("PAIR_NONCE", f_nonce)
c_bytes("PAIR_REVEAL", f_reveal)
c_bytes("PAIR_CONFIRM", f_confirm)
c_bytes("PAIR_DONE", f_done)
c_bytes("K_pair", k_pair)
