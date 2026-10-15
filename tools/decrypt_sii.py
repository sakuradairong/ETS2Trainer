#!/usr/bin/env python3
"""Decrypt an ETS2/ATS ScsC save to its inner payload (dev exploration tool).

Mirrors decryptSave() in cpp/src/saves.cpp: AES-256-CBC with the fixed SCS key,
IV at offset 36, declared size at offset 52, ciphertext from offset 56; the
padded plaintext is either a zlib stream or a known plain format.
Usage: python decrypt_sii.py <game.sii> <out.bin>
"""
import sys
import zlib

KEY = bytes([0x2A, 0x5F, 0xCB, 0x17, 0x91, 0xD2, 0x2F, 0xB6, 0x02, 0x45, 0xB3,
             0xD8, 0x36, 0x9E, 0xD0, 0xB2, 0xC2, 0x73, 0x71, 0x56, 0x3F, 0xBF,
             0x1F, 0x3C, 0x9E, 0xDF, 0x6B, 0x11, 0x82, 0x5A, 0x5D, 0x0A])


def aes_cbc_decrypt(key, iv, data):
    try:
        from Crypto.Cipher import AES
        return AES.new(key, AES.MODE_CBC, iv).decrypt(data)
    except ImportError:
        pass
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        return dec.update(data) + dec.finalize()
    except ImportError:
        pass
    raise SystemExit("no AES backend available (install pycryptodome or cryptography)")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = open(src, "rb").read()
    if raw[:4] != b"ScsC":
        payload = raw  # already plain
    else:
        if len(raw) < 56:
            raise SystemExit("too short")
        declared = int.from_bytes(raw[52:56], "little")
        iv = raw[36:52]
        cipher = raw[56:]
        payload = aes_cbc_decrypt(KEY, iv, cipher)
        pad = payload[-1]
        assert 1 <= pad <= 16 and payload[-pad:] == bytes([pad]) * pad, "bad PKCS7"
        payload = payload[:-pad]
        if len(payload) >= 2 and (payload[0] & 0x0F) == 0x08:
            body = payload[2:-4]
            payload = zlib.decompressobj(-15).decompress(body)
        assert len(payload) == declared, f"size mismatch {len(payload)} != {declared}"
    open(dst, "wb").write(payload)
    head = payload[:8]
    print(f"wrote {len(payload)} bytes to {dst}; head={head!r}")


if __name__ == "__main__":
    main()
