"""What an ESP-IDF app image says about itself: the chip and silicon range it was built for,
its project name, version and ELF hash (esp_app_desc_t), and the public key of the signature
block appended to it (Secure Boot v2 format, the one panel_ota checks for network updates).

Offsets are ESP-IDF's (esp_image_header_t, esp_app_desc_t); tools/panelctl.py reads the same.
"""
import hashlib
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

# esp_chip_id_t
CHIP_IDS = {0x0000: "esp32", 0x0002: "esp32s2", 0x0005: "esp32c3", 0x0009: "esp32s3",
            0x000C: "esp32c2", 0x000D: "esp32c6", 0x0010: "esp32h2", 0x0012: "esp32p4",
            0x0014: "esp32c61", 0x0017: "esp32c5"}
CHIP_NAMES = {"esp32": "ESP32", "esp32s2": "ESP32-S2", "esp32c3": "ESP32-C3",
              "esp32s3": "ESP32-S3", "esp32c2": "ESP32-C2", "esp32c6": "ESP32-C6",
              "esp32h2": "ESP32-H2", "esp32p4": "ESP32-P4", "esp32c61": "ESP32-C61",
              "esp32c5": "ESP32-C5"}
IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
SIG_BLOCK_MAGIC = 0xE7
SIG_BLOCK_RSA = 0x02          # the block's version byte: RSA-3072
SIG_BLOCK_SIZE = 1216
SIG_BLOCK_CRC_AT = 1196       # the CRC-32 of the bytes before it
SECTOR = 4096
NO_MAX_REV = 0xFFFF


def chip_key(name):
    """'ESP32-P4' / 'esp32p4' -> 'esp32p4'."""
    return name.lower().replace("-", "").replace(" ", "")


def chip_name(key):
    return CHIP_NAMES.get(key, key.upper())


def rev_text(rev):
    """Silicon revision as the boot log prints it: 103 -> 'v1.3', 301 -> 'v3.1'."""
    return f"v{rev // 100}.{rev % 100}"


@dataclass
class AppImage:
    chip: str
    min_rev: int
    max_rev: int
    version: str
    project: str
    elf_sha256: str
    idf: str
    signing_key_sha256: str   # "" when unsigned

    def rev_range_text(self):
        if self.max_rev >= NO_MAX_REV:
            return f"silicon {rev_text(self.min_rev)} or newer"
        return f"silicon {rev_text(self.min_rev)} to {rev_text(self.max_rev)}"

    def accepts_rev(self, rev):
        return self.min_rev <= rev <= self.max_rev


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def public_key_digest(n, e):
    """SHA-256 of an RSA-3072 public key as the signature block stores it (little-endian)."""
    return hashlib.sha256(n.to_bytes(384, "little") + e.to_bytes(4, "little")).hexdigest()


def key_digest_from_pem(path):
    """The digest of the key in a PEM file (private or public), or "" when unreadable."""
    try:
        from cryptography.hazmat.primitives import serialization
        data = Path(path).read_bytes()
        try:
            key = serialization.load_pem_private_key(data, None).public_key()
        except (ValueError, TypeError):
            key = serialization.load_pem_public_key(data)
        numbers = key.public_numbers()
        return public_key_digest(numbers.n, numbers.e)
    except Exception:
        return ""


def signature_key_digest(data):
    """The public key digest of the image's first Secure Boot v2 (RSA-3072) signature block.

    The blocks sit in the signed image's last 4 KiB sector, up to three of 1216 bytes, each with a
    CRC-32 of its first 1196 bytes: that is where and how espsecure finds them. Bytes that merely
    look like a block's start can be anywhere in an app (two 2026-09-24 builds had them at 4 KiB
    boundaries, and taking the first was a bug), so only the last sector counts, and a block only
    when its CRC is right."""
    if len(data) < SECTOR or len(data) % SECTOR:
        return ""
    sector = data[-SECTOR:]
    for n in range(3):
        blk = sector[n * SIG_BLOCK_SIZE:(n + 1) * SIG_BLOCK_SIZE]
        if blk[0] != SIG_BLOCK_MAGIC or blk[1] != SIG_BLOCK_RSA:
            continue
        if zlib.crc32(blk[:SIG_BLOCK_CRC_AT]) != struct.unpack_from("<I", blk, SIG_BLOCK_CRC_AT)[0]:
            continue
        return hashlib.sha256(blk[36:36 + 388]).hexdigest()
    return ""


def verified_key_digest(data):
    """The public key digest of the first Secure Boot v2 (RSA-3072) block whose signature checks
    out against the bytes before the signature sector, or "" when none does: what `espsecure
    verify_signature --version 2` checks (RSA-PSS, SHA-256, a 32-byte salt), with the key the block
    carries. Whether that key is trusted is the caller's question (fwserver.TRUSTED_KEYS).
    signature_key_digest() only reads which key a block names; this proves the file wasn't changed
    after it was signed. Needs `cryptography` (esptool's own dependency)."""
    if len(data) < SECTOR or len(data) % SECTOR:
        return ""
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
    digest = hashlib.sha256(data[:-SECTOR]).digest()
    sector = data[-SECTOR:]
    for n in range(3):
        blk = sector[n * SIG_BLOCK_SIZE:(n + 1) * SIG_BLOCK_SIZE]
        if blk[0] != SIG_BLOCK_MAGIC or blk[1] != SIG_BLOCK_RSA:
            continue
        if zlib.crc32(blk[:SIG_BLOCK_CRC_AT]) != struct.unpack_from("<I", blk, SIG_BLOCK_CRC_AT)[0]:
            continue
        if blk[4:36] != digest:
            continue
        # The block, little-endian: digest 4..36, n 36..420, e 420..424, rinv, m', signature 812..1196.
        try:
            key = rsa.RSAPublicNumbers(int.from_bytes(blk[420:424], "little"),
                                       int.from_bytes(blk[36:420], "little")).public_key()
            key.verify(blk[812:1196][::-1], digest,
                       padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                       utils.Prehashed(hashes.SHA256()))
        except (InvalidSignature, ValueError):
            continue
        return hashlib.sha256(blk[36:36 + 388]).hexdigest()
    return ""


def parse_app(data):
    if len(data) < 256 or data[0] != IMAGE_MAGIC:
        raise ValueError("not an ESP32 app image")
    if struct.unpack_from("<I", data, 32)[0] != APP_DESC_MAGIC:
        raise ValueError("an image without an app description (a bootloader or a whole-flash dump?)")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id not in CHIP_IDS:
        raise ValueError(f"unknown chip id {chip_id:#x}")
    return AppImage(chip=CHIP_IDS[chip_id],
                    min_rev=struct.unpack_from("<H", data, 15)[0],
                    max_rev=struct.unpack_from("<H", data, 17)[0],
                    version=_cstr(data[48:80]), project=_cstr(data[80:112]),
                    elf_sha256=data[176:208].hex(), idf=_cstr(data[144:176]),
                    signing_key_sha256=signature_key_digest(data))
