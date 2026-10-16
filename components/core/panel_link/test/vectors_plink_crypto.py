"""Known answers for test_plink_crypto.c, checked independently with Python's `cryptography`
(OpenSSL) and the standard library (docs/features/11, LINK-003, LINK-004, LINK-015).

Every vector the test takes from a standard (RFC 4231, RFC 5869, RFC 7748, RFC 3610) is written
here as published and recomputed; the script fails when one doesn't match. It also prints the
SHA-256 of the 200-byte message the streaming test splits. Run it from anywhere:

    python components/core/panel_link/test/vectors_plink_crypto.py
"""

import hashlib
import hmac

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF, HKDFExpand
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def h(s):
    return bytes.fromhex(s.replace(" ", ""))


def check(name, got, want):
    if got != want:
        raise SystemExit(f"{name}: got {got.hex()}, want {want.hex()}")
    print(f"ok  {name}: {got.hex()}")


# ---- RFC 4231: HMAC-SHA-256 test cases 1-7 (5 truncated to 128 bits) --------------------------
RFC4231 = [
    (1, b"\x0b" * 20, b"Hi There",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
    (2, b"Jefe", b"what do ya want for nothing?",
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
    (3, b"\xaa" * 20, b"\xdd" * 50,
     "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
    (4, h("0102030405060708090a0b0c0d0e0f10111213141516171819"), b"\xcd" * 50,
     "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"),
    (5, b"\x0c" * 20, b"Test With Truncation", "a3b6167473100ee06e0c796c2955552b"),
    (6, b"\xaa" * 131, b"Test Using Larger Than Block-Size Key - Hash Key First",
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
    (7, b"\xaa" * 131,
     b"This is a test using a larger than block-size key and a larger than block-size data. "
     b"The key needs to be hashed before being used by the HMAC algorithm.",
     "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"),
]
for case, key, data, want in RFC4231:
    mac = hmac.new(key, data, hashlib.sha256).digest()
    check(f"RFC 4231 case {case}", mac[: len(h(want))], h(want))

# Keys around SHA-256's 64-byte block (63 and 64 are padded, 65 is hashed first), and the empty
# key (HKDF's default salt: 32 zero bytes, the same once padded): computed, not from a standard.
for n in (0, 63, 64, 65):
    mac = hmac.new(b"\xa5" * n, b"plink v1 hmac", hashlib.sha256).digest()
    print(f"    HMAC(a5 x {n}, 'plink v1 hmac'): {mac.hex()}")
check("HMAC empty key = 32 zero bytes", hmac.new(b"", b"x", hashlib.sha256).digest(),
      hmac.new(b"\x00" * 32, b"x", hashlib.sha256).digest())

# ---- RFC 5869 appendix A.1-A.3: HKDF-SHA256 --------------------------------------------------
RFC5869 = [
    ("A.1", b"\x0b" * 22, h("000102030405060708090a0b0c"), h("f0f1f2f3f4f5f6f7f8f9"), 42,
     "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
    ("A.2", bytes(range(0x00, 0x50)), bytes(range(0x60, 0xB0)), bytes(range(0xB0, 0x100)), 82,
     "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
     "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
     "cc30c58179ec3e87c14c01d5c1f3434f1d87"),
    ("A.3", b"\x0b" * 22, b"", b"", 42,
     "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
]
for name, ikm, salt, info, length, prk_hex, okm_hex in RFC5869:
    prk = hmac.new(salt if salt else b"\x00" * 32, ikm, hashlib.sha256).digest()
    check(f"RFC 5869 {name} PRK", prk, h(prk_hex))
    okm = HKDFExpand(algorithm=hashes.SHA256(), length=length, info=info).derive(prk)
    check(f"RFC 5869 {name} OKM", okm, h(okm_hex))
    okm2 = HKDF(algorithm=hashes.SHA256(), length=length, salt=salt or None, info=info).derive(ikm)
    check(f"RFC 5869 {name} OKM (one call)", okm2, h(okm_hex))

# HKDF-Expand's longest output (255 * 32 bytes) from A.1's PRK and info: its last block.
okm = HKDFExpand(algorithm=hashes.SHA256(), length=255 * 32, info=h("f0f1f2f3f4f5f6f7f8f9")).derive(
    h(RFC5869[0][5]))
print(f"    HKDF-Expand A.1 PRK/info, 8160 bytes, last 32: {okm[-32:].hex()}")


# ---- RFC 7748 --------------------------------------------------------------------------------
def x25519(k, u):
    return X25519PrivateKey.from_private_bytes(k).exchange(X25519PublicKey.from_public_bytes(u))


check("RFC 7748 5.2 #1",
      x25519(h("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
             h("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")),
      h("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"))
check("RFC 7748 5.2 #2",
      x25519(h("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
             h("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")),
      h("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"))

ALICE = h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
ALICE_PUB = h("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
BOB = h("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
BOB_PUB = h("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
SHARED = h("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")


def pub(k):
    return X25519PrivateKey.from_private_bytes(k).public_key().public_bytes(Encoding.Raw,
                                                                          PublicFormat.Raw)


check("RFC 7748 6.1 Alice's public key", pub(ALICE), ALICE_PUB)
check("RFC 7748 6.1 Bob's public key", pub(BOB), BOB_PUB)
check("RFC 7748 6.1 shared (Alice)", x25519(ALICE, BOB_PUB), SHARED)
check("RFC 7748 6.1 shared (Bob)", x25519(BOB, ALICE_PUB), SHARED)

# Points of small order and their aliases (u >= p): X25519 gives all zero whatever the scalar,
# which OpenSSL refuses (ValueError), as plink_x25519_shared must.
LOW_ORDER = [
    "0000000000000000000000000000000000000000000000000000000000000000",  # 0
    "0100000000000000000000000000000000000000000000000000000000000000",  # 1
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",  # order 8
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",  # order 8
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # p - 1
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # p (= 0)
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",  # p + 1 (= 1)
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",  # order 8, top bit set
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",  # order 8, top bit set
]
for u in LOW_ORDER:
    try:
        x25519(ALICE, h(u))
    except ValueError:
        print(f"ok  low-order point refused: {u}")
    else:
        raise SystemExit(f"low-order point {u} was not refused")

# ---- RFC 3610 packet vector #1 (M = 8, L = 2) -----------------------------------------------
key = h("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf")
nonce = h("00000003020100a0a1a2a3a4a5")
packet = h("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e")
out = AESCCM(key, tag_length=8).encrypt(nonce, packet[8:], packet[:8])
check("RFC 3610 packet #1", packet[:8] + out,
      h("0001020304050607588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0"))

# ---- SHA-256 of the streaming test's message: byte i = (7 i + 3) mod 256, 200 bytes -----------
msg = bytes((7 * i + 3) % 256 for i in range(200))
print(f"    SHA-256 of the 200-byte pattern: {hashlib.sha256(msg).hexdigest()}")
digest = hashes.Hash(hashes.SHA256())
digest.update(msg)
check("SHA-256 of the 200-byte pattern (cryptography)", digest.finalize(),
      hashlib.sha256(msg).digest())
print("all vectors match")
