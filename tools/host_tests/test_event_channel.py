#!/usr/bin/env python3
"""Sender-side model for test_event_channel.c (independent implementation).

Keys as Shairport Sync's server cipher channel 4 (pair_homekit.c cipher_new):
the receiver writes with "Events-Write-Encryption-Key" and reads with
"Events-Read-Encryption-Key"; the sender does the opposite.
"""
import subprocess
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

IKM = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
             0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
             0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10])


def key(info):
    return HKDF(algorithm=hashes.SHA512(), length=32, salt=b"Events-Salt",
                info=info).derive(IKM)


def nonce(n):
    return b"\0" * 4 + n.to_bytes(8, "little")


def expected_message():
    head = (b"POST /command RTSP/1.0\r\nContent-Length: 2500\r\n"
            b"Content-Type: application/x-apple-binary-plist\r\n\r\n")
    return head + bytes(((i * 31 + 7) & 0xFF) for i in range(2500))


def main(exe, tmp):
    # DataStream receiver-side key orientation. The sender opens the socket,
    # therefore receiver encrypt = Input key and receiver decrypt = Output key.
    seed = 15014746994705656022
    keys_path = f"{tmp}/datastream_keys.bin"
    subprocess.run([exe, "datakeys", str(seed), keys_path], check=True)
    got = open(keys_path, "rb").read()
    salt = b"DataStream-Salt" + str(seed).encode()
    enc = HKDF(algorithm=hashes.SHA512(), length=32, salt=salt,
               info=b"DataStream-Input-Encryption-Key").derive(IKM)
    dec = HKDF(algorithm=hashes.SHA512(), length=32, salt=salt,
               info=b"DataStream-Output-Encryption-Key").derive(IKM)
    assert got == enc + dec, "DataStream key derivation/orientation mismatch"
    print("DataStream keys: ok")

    for orientation, rx_info, tx_info in (
            ("1", b"Events-Write-Encryption-Key", b"Events-Read-Encryption-Key"),
            ("0", b"Events-Read-Encryption-Key", b"Events-Write-Encryption-Key")):
        sealed = f"{tmp}/event_sealed_{orientation}.bin"
        subprocess.run([exe, "seal", orientation, sealed], check=True)
        data = open(sealed, "rb").read()
        aead = ChaCha20Poly1305(key(rx_info))
        pos, n, plain = 0, 0, b""
        while pos < len(data):
            blen = data[pos] | (data[pos + 1] << 8)
            aad = data[pos:pos + 2]
            plain += aead.decrypt(nonce(n), data[pos + 2:pos + 2 + blen + 16], aad)
            pos += 2 + blen + 16
            n += 1
        assert plain == expected_message(), "event message mismatch"
        # Sender's reply, as a real sender answers POST /command.
        reply = b"RTSP/1.0 200 OK\r\nServer: AirTunes/900.0\r\nContent-Length: 0\r\n\r\n"
        aad = len(reply).to_bytes(2, "little")
        frame = aad + ChaCha20Poly1305(key(tx_info)).encrypt(nonce(0), reply, aad)
        reply_path = f"{tmp}/event_reply_{orientation}.bin"
        open(reply_path, "wb").write(frame)
        subprocess.run([exe, "open", orientation, reply_path], check=True)
        print(f"orientation {orientation}: sender decrypted {n} frames, "
              "receiver decrypted the reply: ok")
    print("event channel: all tests passed")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
