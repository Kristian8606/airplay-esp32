"""Synthetic iPhone MediaRemote DEVICE_INFO comm payload + reply validator."""
import plistlib, sys

def varint(v):
    out = b""
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])

def key(field, wire):
    return varint((field << 3) | wire)

def s(field, text):
    data = text.encode() if isinstance(text, str) else text
    return key(field, 2) + varint(len(data)) + data

def u(field, v):
    return key(field, 0) + varint(v)

def parse(buf):
    pos, fields = 0, []
    while pos < len(buf):
        k = 0; shift = 0
        while True:
            c = buf[pos]; pos += 1
            k |= (c & 0x7f) << shift; shift += 7
            if not c & 0x80: break
        field, wire = k >> 3, k & 7
        if wire == 0:
            v = 0; shift = 0
            while True:
                c = buf[pos]; pos += 1
                v |= (c & 0x7f) << shift; shift += 7
                if not c & 0x80: break
            fields.append((field, v))
        elif wire == 2:
            n = 0; shift = 0
            while True:
                c = buf[pos]; pos += 1
                n |= (c & 0x7f) << shift; shift += 7
                if not c & 0x80: break
            fields.append((field, buf[pos:pos + n])); pos += n
        else:
            raise ValueError("wire %d" % wire)
    return fields

def make_request(path):
    devinfo = (s(1, "8E1C2D3A-0000-4000-8000-0123456789AB") + s(2, "Kristian's iPhone")
               + s(3, "iPhone") + s(4, "24C5057p") + s(5, "com.apple.mediaremoted")
               + s(6, "1234.5.6") + u(7, 1) + u(8, 120) + u(9, 1) + u(10, 1)
               + s(11, "com.apple.Music") + u(12, 1) + u(13, 1) + s(17, "8E1C2D3A-UID")
               + u(19, 1) + u(20, 1) + s(24, "GROUP-UID-0000") + s(25, "Kristian's iPhone")
               + s(15, bytes([0xF0, 0x9E, 0x9E, 0x0E, 0xE2, 0x42])))
    msg = u(1, 15) + s(2, "5B0F2C6E-7D0B-4B3F-9C9A-1A2B3C4D5E6F") + s(20, devinfo)
    data = varint(len(msg)) + msg
    plist = plistlib.dumps({"params": {"data": data}}, fmt=plistlib.FMT_BINARY)
    open(path, "wb").write(plist)

def check_reply(req_path, reply_path):
    req = plistlib.loads(open(req_path, "rb").read())["params"]["data"]
    rep = plistlib.loads(open(reply_path, "rb").read())
    data = rep["params"]["data"]
    n = 0; shift = 0; pos = 0
    while True:
        c = data[pos]; pos += 1
        n |= (c & 0x7f) << shift; shift += 7
        if not c & 0x80: break
    assert n == len(data) - pos, "framing"
    msg = dict(parse(data[pos:]))
    reqmsg = dict(parse(req[1:] if req[0] < 0x80 else req[2:]))
    assert msg[1] == 15, "type"
    assert msg[2] == reqmsg[2], "identifier echoed"
    body = dict(parse(msg[20]))
    assert body[1] == b"F09E9E0E-E240-4FE9-9582-8388ABEE73A5"
    assert body[2] == b"ESP32 AirPlay" and body[3] == b"HomePod mini"
    assert body[4] == b"27.2" and body[5] == b"com.apple.mediaremoted"
    assert body[6] == b"1234.5.6" and body[7] == 1
    print("DEVICE_INFO reply plist/protobuf: ok")

if __name__ == "__main__":
    if sys.argv[1] == "make":
        make_request(sys.argv[2])
    else:
        check_reply(sys.argv[2], sys.argv[3])
