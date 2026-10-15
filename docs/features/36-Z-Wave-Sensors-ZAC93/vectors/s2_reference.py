#!/usr/bin/env python3
"""Reference check for docs/features/36 security.md (Z-Wave S2/S0 recipes).

Written from security.md alone, not from Z-Wave JS: it recomputes every known-answer vector in
security.md section 11 and exits non-zero on the first mismatch. The expected values themselves
came from running Z-Wave JS's crypto code (commit fac3872), so a pass means the page describes
that code correctly. Needs Python 3.9+ and the `cryptography` package (the ESP-IDF venv has it:
C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe).

Run: python docs/features/36-Z-Wave-Sensors-ZAC93/vectors/s2_reference.py
"""
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


def aes(key: bytes, block: bytes) -> bytes:
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return e.update(block) + e.finalize()


def xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def shl1(b: bytes) -> bytes:
    n = (int.from_bytes(b, "big") << 1) & ((1 << 128) - 1)
    return n.to_bytes(16, "big")


def cmac(key: bytes, msg: bytes) -> bytes:
    L = aes(key, bytes(16))
    k1 = shl1(L)
    if L[0] & 0x80:
        k1 = xor(k1, bytes(15) + b"\x87")
    k2 = shl1(k1)
    if k1[0] & 0x80:
        k2 = xor(k2, bytes(15) + b"\x87")
    n = max(1, (len(msg) + 15) // 16)
    blocks = [msg[i * 16:(i + 1) * 16] for i in range(n)]
    if msg and len(msg) % 16 == 0:
        last = xor(blocks[-1], k1)
    else:
        padded = blocks[-1] + b"\x80" + bytes(15 - len(blocks[-1]))
        last = xor(padded, k2)
    x = bytes(16)
    for b in blocks[:-1]:
        x = aes(key, xor(x, b))
    return aes(key, xor(x, last))


C33, C26 = b"\x33" * 16, b"\x26" * 16
TE, NK, EI = b"\x88" * 15, b"\x55" * 15, b"\x88" * 15


def network_key_expand(pnk: bytes):
    t1 = cmac(pnk, NK + b"\x01")
    t2 = cmac(pnk, t1 + NK + b"\x02")
    t3 = cmac(pnk, t2 + NK + b"\x03")
    t4 = cmac(pnk, t3 + NK + b"\x04")
    return t1, t2 + t3, t4  # KeyCCM, PString, KeyMPAN


def temp_keys(shared: bytes, pub_include: bytes, pub_join: bytes):
    prk = cmac(C33, shared + pub_include + pub_join)
    t1 = cmac(prk, TE + b"\x01")
    t2 = cmac(prk, t1 + TE + b"\x02")
    t3 = cmac(prk, t2 + TE + b"\x03")
    return prk, t1, t2 + t3


def mei(sender_ei: bytes, receiver_ei: bytes):
    nprk = cmac(C26, sender_ei + receiver_ei)
    t1 = cmac(nprk, EI + b"\x00" + EI + b"\x01")
    t2 = cmac(nprk, t1 + EI + b"\x02")
    return nprk, t1 + t2


class CtrDrbg:
    def __init__(self, entropy32: bytes, ps32: bytes):
        self.key, self.v = bytes(16), bytes(16)
        self._update(xor(entropy32, ps32))

    def _inc(self):
        self.v = ((int.from_bytes(self.v, "big") + 1) % (1 << 128)).to_bytes(16, "big")

    def _update(self, data32: bytes = bytes(32)):
        self._inc()
        t1 = aes(self.key, self.v)
        self._inc()
        t2 = aes(self.key, self.v)
        t = xor(t1 + t2, data32)
        self.key, self.v = t[:16], t[16:]

    def generate(self, n: int) -> bytes:
        out = b""
        while len(out) < n:
            self._inc()
            out += aes(self.key, self.v)
        self._update()
        return out[:n]


def dsk_string(pub: bytes) -> str:
    return "-".join(f"{int.from_bytes(pub[i:i + 2], 'big'):05d}" for i in range(0, 16, 2))


def s2_aad(src: int, dst: int, home_id: int, msg_len: int, unencrypted: bytes) -> bytes:
    w = 2 if src >= 256 or dst >= 256 else 1
    return (src.to_bytes(w, "big") + dst.to_bytes(w, "big") + home_id.to_bytes(4, "big")
            + msg_len.to_bytes(2, "big") + unencrypted)


def s2_frame(key, nonce, src, dst, home_id, unencrypted, plaintext):
    msg_len = 2 + len(unencrypted) + len(plaintext) + 8
    aad = s2_aad(src, dst, home_id, msg_len, unencrypted)
    ct_tag = AESCCM(key, tag_length=8).encrypt(nonce, plaintext, aad)
    return msg_len, aad, ct_tag[:-8], ct_tag[-8:], b"\x9f\x03" + unencrypted + ct_tag


def ofb(key: bytes, iv: bytes, data: bytes) -> bytes:
    stream, s = b"", iv
    while len(stream) < len(data):
        s = aes(key, s)  # S1 = E(K, IV), Si = E(K, Si-1)
        stream += s
    return xor(data, stream)


def cbc_mac8(key: bytes, data: bytes) -> bytes:
    padded = data + bytes((-len(data)) % 16)
    e = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).encryptor()
    ct = e.update(padded) + e.finalize()
    return ct[-16:][:8]


def crc16_ccitt(data: bytes, crc: int = 0x1D0F) -> int:
    for byte in data:
        for bit in range(7, -1, -1):
            flag = bool(byte & (1 << bit)) != bool(crc & 0x8000)
            crc = (crc << 1) & 0xFFFF
            if flag:
                crc ^= 0x1021
    return crc


def main() -> int:
    got = {}
    h = bytes.fromhex
    pnk = bytes(range(16))
    kccm, ps, kmpan = network_key_expand(pnk)
    got["NK.keyCCM"], got["NK.PS"], got["NK.keyMPAN"] = kccm, ps, kmpan

    a = X25519PrivateKey.from_private_bytes(h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"))
    b = X25519PrivateKey.from_private_bytes(h("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"))
    apub = a.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    bpub = b.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    shared = a.exchange(X25519PublicKey.from_public_bytes(bpub))
    got["X.A.pub"], got["X.B.pub"], got["X.shared"] = apub, bpub, shared
    prk, tk, tps = temp_keys(shared, apub, bpub)
    got["TK.PRK"], got["TK.keyCCM"], got["TK.PS"] = prk, tk, tps
    got["DSK"] = dsk_string(bpub[:16])

    sei, rei = bytes(range(0x10, 0x20)), bytes(range(0x20, 0x30))
    nprk, m = mei(sei, rei)
    got["SPAN.noncePRK"], got["SPAN.MEI"] = nprk, m
    rng = CtrDrbg(m, ps)
    n1, n2, n3 = (rng.generate(16)[:13] for _ in range(3))
    got["SPAN.nonce1"], got["SPAN.nonce2"], got["SPAN.nonce3"] = n1, n2, n3

    unenc = bytes([0x55, 0x01, 0x12, 0x41]) + sei
    ln, aad, ct, tag, frame = s2_frame(kccm, n1, 2, 1, 0xC0FFEE01, unenc, h("7105000000ff061600"))
    got["S2.msgLen"], got["S2.AAD"], got["S2.ciphertext"], got["S2.tag"], got["S2.frame"] = ln, aad, ct, tag, frame
    got["S2.frame2"] = s2_frame(kccm, n2, 2, 1, 0xC0FFEE01, bytes([0x56, 0x00]), h("8003ff"))[4]

    got["CMAC.nist.empty"] = cmac(h("2b7e151628aed2a6abf7158809cf4f3c"), b"")

    s0 = bytes(range(16))
    ak, ek = aes(s0, b"\x55" * 16), aes(s0, b"\xaa" * 16)
    got["S0.authKey"], got["S0.encKey"] = ak, ek
    got["S0.tempKeyAuth"], got["S0.tempKeyEnc"] = aes(bytes(16), b"\x55" * 16), aes(bytes(16), b"\xaa" * 16)
    sn, rn = bytes(range(1, 9)), bytes(range(0xA1, 0xA9))
    ct0 = ofb(ek, sn + rn, h("002002"))
    mac = cbc_mac8(ak, sn + rn + bytes([0x81, 1, 2, len(ct0)]) + ct0)
    got["S0.ciphertext"], got["S0.mac"] = ct0, mac
    got["S0.frame"] = b"\x98\x81" + sn + ct0 + rn[:1] + mac
    got["S0.realMAC"] = cbc_mac8(h("c5fe1ca17d36c992731a0c0c468c1ef9"),
                                 h("ddd360c382a437514392826cbba0b3128114010cf3fb762d6e82126681c18597"))
    got["CRC16"] = crc16_ccitt(h("56012002"))

    expected = {
        "NK.keyCCM": "27f90dd6d74697cba54eb11a0ee80617",
        "NK.keyMPAN": "4a46b6e046c129bc72f737e57e9b5786",
        "NK.PS": "aa3b035d8f802f368d6482f4299a2b1255eed934805c397bb72c47c61ff7acfa",
        "X.A.pub": "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
        "X.B.pub": "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
        "X.shared": "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
        "TK.PRK": "c7219805a4bd07bbe45915e8530c5975",
        "TK.keyCCM": "7dd067ed9c4fa4e9db2ef5bb0ad9a7b2",
        "TK.PS": "d62e2993be086f2ad3aba4d45cef3418533fe70df5139984871b5cc2d6d2c6df",
        "DSK": "56990-56189-31613-49588-54107-25026-60644-13623",
        "SPAN.noncePRK": "91d5df52e46bb3fe8673a199e839cbc8",
        "SPAN.MEI": "f15b458d77005caa77e7ca5eb120e60793b1e2c78e751d71cd23eee80a8a83d6",
        "SPAN.nonce1": "f8648b2a31d81dea7bb3e02cef",
        "SPAN.nonce2": "4122a17ea64ebab751c63fa29c",
        "SPAN.nonce3": "edd75c79138b6bf058474a9193",
        "S2.AAD": "0201c0ffee01002755011241101112131415161718191a1b1c1d1e1f",
        "S2.msgLen": 39,
        "S2.ciphertext": "ee50c2d601c41ad285",
        "S2.tag": "02add758929a40ad",
        "S2.frame": "9f0355011241101112131415161718191a1b1c1d1e1fee50c2d601c41ad28502add758929a40ad",
        "S2.frame2": "9f035600c7d38ef47c7b1463532a4b",
        "CMAC.nist.empty": "bb1d6929e95937287fa37d129b756746",
        "S0.authKey": "a5548f94150184202f8200aaa9d02d56",
        "S0.encKey": "fbd49b091a38f87ad5c3ee2580ba414c",
        "S0.ciphertext": "7ec72f",
        "S0.mac": "d55d5cac51d9831c",
        "S0.frame": "988101020304050607087ec72fa1d55d5cac51d9831c",
        "S0.tempKeyAuth": "9adae054f63dfaff5ea18e45edf6ea6f",
        "S0.tempKeyEnc": "8522717d3ad1fbfeafa1ceaafdf56565",
        "S0.realMAC": "2bc20a8aa9bbb371",
        "CRC16": 0x4D26,
    }
    bad = 0
    for name, want in expected.items():
        have = got[name]
        if isinstance(have, bytes):
            have = have.hex()
        ok = have == want
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} {name:16} {have}")
    print("all vectors match" if not bad else f"{bad} mismatch(es)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
