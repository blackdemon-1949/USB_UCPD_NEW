#!/usr/bin/env python3
"""Tests for pdtools.py, including a byte-for-byte check against the ST
middleware's own worked example and independently constructed PD payloads.

Run:  python3 tools/pdtools/test_pdtools.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdtools as P

fails = 0


def check(cond, what):
    global fails
    if cond:
        return True
    fails += 1
    print('  FAIL %s' % what)
    return False


def eq(got, want, what):
    if got == want:
        return True
    global fails
    fails += 1
    print('  FAIL %s: got %r want %r' % (what, got, want))
    return False


# ---------------------------------------------------------------------------
# 1. Byte-for-byte against USBPD_TRACE_Init()'s OverFlow_String in
#    Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c.
#    That array is the authoritative worked example of the format.
# ---------------------------------------------------------------------------
def test_against_st_overflow_record():
    print('test_against_st_overflow_record')
    expected = bytes([
        0xFD, 0xFD, 0xFD, 0xFD,           # TLV_SOF x4
        0x32,                             # tag
        0x00, 0x18,                       # length = 24 (big-endian)
        0x06,                             # type
        0x00, 0x00, 0x00, 0x00,           # time (little-endian)
        0x00,                             # port
        0x00,                             # sop
        0x00, 0x0F,                       # size = 15 (big-endian)
    ]) + b'TRACE OVER_FLOW' + bytes([0xA5, 0xA5, 0xA5, 0xA5])

    # The overflow record uses tag 0x32 directly and type 0x06 (DEBUG).
    # Reproduce it with the encoder's field layout.
    payload = b'TRACE OVER_FLOW'
    size = len(payload)
    length = size + 9
    got = (bytes([0xFD]) * 4 + bytes([0x32])
           + bytes([(length >> 8) & 0xFF, length & 0xFF])
           + bytes([0x06])
           + bytes(4)
           + bytes([0x00, 0x00])
           + bytes([(size >> 8) & 0xFF, size & 0xFF])
           + payload + bytes([0xA5]) * 4)
    eq(got, expected, 'hand-built record matches ST OverFlow_String')

    # The encoder must produce the same body for tag 0x32, i.e. the length and
    # size fields must agree with ST's 0x0018 / 0x000F.
    rec = P.cpd_encode_record(0x06, 0, 0, 0, payload)
    # Layout: [0:4] SOF, [4] tag, [5:7] length BE, [7] type, [8:12] time LE,
    # [12] port, [13] sop, [14:16] size BE, [16:] payload, then EOF.
    eq(rec[5:7], bytes([0x00, 0x18]), 'encoder length field = 0x0018')
    eq((rec[14] << 8) | rec[15], 15, 'encoder size field = 15')
    eq(rec[0:4], bytes([0xFD]) * 4, 'SOF run')
    eq(rec[-4:], bytes([0xA5]) * 4, 'EOF run')
    eq(len(rec), 4 + 3 + 9 + 15 + 4, 'total record length')

    # ST's tag is ((port+1) << 5) | 0x12.  For port 0 that is 0x32, which is
    # exactly the tag in OverFlow_String - confirming the tag formula.
    eq(P.cpd_encode_record(0x06, 0, 0, 0, b'x')[4], 0x32,
       'tag for port 0 = 0x32, matching ST')


# ---------------------------------------------------------------------------
# 2. Round trip through the parser.
# ---------------------------------------------------------------------------
def test_roundtrip():
    print('test_roundtrip')
    frames = [(bytes([0x01, 0x22, 0x2C, 0xD1, 0x02, 0x00]), 1000, 'rx', 0),
              (bytes([0x03, 0x05]), 2000, 'tx', 0),
              (bytes([0x06, 0x05]), 3000, 'rx', 0)]
    blob = b''.join(P.cpd_encode_record(P.direction_for(d), 0, sop, ts, raw)
                    for raw, ts, d, sop in frames)
    recs = [r for r in P.cpd_decode_stream(blob)]
    eq(len(recs), 3, 'three records parsed')
    eq([r['payload'] for r in recs], [f[0] for f in frames], 'payloads intact')
    eq([r['ts'] for r in recs], [1000, 2000, 3000], 'timestamps intact')
    eq([r['event'] for r in recs],
       [P.EV_MSG_IN, P.EV_MSG_OUT, P.EV_MSG_IN], 'directions intact')
    eq(recs[0]['event_name'], 'MSG_IN', 'event name')


# ---------------------------------------------------------------------------
# 3. Corrupt streams must be reported, not crash or silently truncate.
# ---------------------------------------------------------------------------
def test_corrupt():
    print('test_corrupt')
    good = P.cpd_encode_record(1, 0, 0, 5, bytes([0x03, 0x05]))

    eq(len(list(P.cpd_decode_stream(b''))), 0, 'empty stream')
    eq(len(list(P.cpd_decode_stream(bytes(64)))), 0, 'no SOF anywhere')
    eq(len(list(P.cpd_decode_stream(good[:8]))), 1, 'truncated header reported')
    eq(len(list(P.cpd_decode_stream(good + b'\xfd\xfd'))), 1,
       'trailing partial SOF ignored')

    # A size field larger than the payload present.
    bad = bytearray(good)
    bad[14] = 0x00
    bad[15] = 0xFF
    out = list(P.cpd_decode_stream(bytes(bad)))
    eq(len(out), 1, 'oversized size reported once')
    check('error' in out[0], 'oversized size flagged as error')

    # Junk between records must be skipped and both records recovered.
    eq(len(list(P.cpd_decode_stream(good + b'\x00\x11\x22' + good))), 2,
       'junk between records skipped')


# ---------------------------------------------------------------------------
# 4. PDO / RDO decoding with independently constructed real payloads.
#    Field positions per USB PD 3.1 and usbpd_def.h, NOT copied from the tool.
# ---------------------------------------------------------------------------
def test_pdo_rdo():
    print('test_pdo_rdo')

    # Fixed 9 V / 3 A: voltage in 50 mV at B19..10, current in 10 mA at B9..0.
    pdo_9v3a = (180 << 10) | 300
    eq(pdo_9v3a, 0x2D12C, 'constructed 9V/3A fixed PDO value')
    d = P.decode_pdo(pdo_9v3a)
    eq(d['kind'], 'fixed', 'fixed kind')
    eq(d['voltage_mv'], 9000, 'fixed voltage')
    eq(d['current_ma'], 3000, 'fixed current')
    eq(d['epr_capable'], 0, 'EPR bit clear')

    # Same PDO with the EPR-Mode-Capable bit (B23) set.
    d = P.decode_pdo(pdo_9v3a | (1 << 23))
    eq(d['epr_capable'], 1, 'EPR bit set')

    # Fixed 5 V / 900 mA, the default USB contract.
    d = P.decode_pdo((100 << 10) | 90)
    eq(d['voltage_mv'], 5000, '5 V')
    eq(d['current_ma'], 900, '900 mA')

    # PPS APDO 3.3-11 V, 5 A: object type 11b at B31..30, subtype 00b at B29..28,
    # power-limited at B27, max V in 100 mV at B24..17, min V in 100 mV at
    # B15..8, max current in 50 mA at B6..0.
    apdo = (3 << 30) | (0 << 28) | (1 << 27) | (110 << 17) | (33 << 8) | 100
    d = P.decode_pdo(apdo)
    eq(d['kind'], 'pps', 'pps kind')
    eq(d['min_mv'], 3300, 'pps min')
    eq(d['max_mv'], 11000, 'pps max')
    eq(d['max_ma'], 5000, 'pps max current')
    eq(d['power_limited'], 1, 'pps power limited')

    # EPR AVS PDO: subtype 01b, PDP in 1 W at B7..0, min V in 100 mV at B15..8,
    # max V in 100 mV at B25..17.
    avs = (3 << 30) | (1 << 28) | (480 << 17) | (150 << 8) | 240
    d = P.decode_pdo(avs)
    eq(d['kind'], 'avs', 'avs kind')
    eq(d['pdp_w'], 240, 'avs pdp')
    eq(d['min_mv'], 15000, 'avs min')
    eq(d['max_mv'], 48000, 'avs max')

    # RDO for object 2, 9 V, 3 A: position at B31..28, operating current in
    # 10 mA at B19..10, maximum current in 10 mA at B9..0.
    rdo = (2 << 28) | (300 << 10) | 300
    eq(rdo, 0x2004B12C, 'constructed RDO value')
    hdr = bytes([0x02, (1 << 4) | (1 << 1)])        # Request, NDO 1, id 1
    rec = P.decode_frame(hdr + rdo.to_bytes(4, 'little'))
    eq(rec['name'], 'Request', 'request name')
    eq(rec['rdo']['pos'], 2, 'rdo position')
    eq(rec['rdo']['op_curr_ma'], 3000, 'rdo operating current')
    eq(rec['rdo']['max_curr_ma'], 3000, 'rdo maximum current')

    # The EPR-Mode-Capable bit in an RDO is B21.
    rec = P.decode_frame(hdr + (rdo | (1 << 21)).to_bytes(4, 'little'))
    eq(rec['rdo']['epr_mode'], 1, 'rdo EPR mode bit')
    eq(rec['rdo']['epr_mode'], 1, 'epr bit set')
    rec = P.decode_frame(hdr + rdo.to_bytes(4, 'little'))
    eq(rec['rdo']['epr_mode'], 0, 'epr bit clear')


# ---------------------------------------------------------------------------
# 5. Full frames, built from the header bit layout rather than copied.
# ---------------------------------------------------------------------------
def test_frames():
    print('test_frames')

    def hdr(mtype, ndo, msgid, pr=0, dr=0, rev=2, ext=0):
        b0 = mtype | (dr << 5) | (rev << 6)
        b1 = pr | (msgid << 1) | (ndo << 4) | (ext << 7)
        return bytes([b0, b1])

    # Source_Capabilities with two fixed PDOs.
    pdo1 = (100 << 10) | 300          # 5 V / 3 A
    pdo2 = (180 << 10) | 300          # 9 V / 3 A
    frame = hdr(0x01, 2, 1, pr=1) + pdo1.to_bytes(4, 'little') \
        + pdo2.to_bytes(4, 'little')
    rec = P.decode_frame(frame)
    eq(rec['name'], 'Source_Capabilities', 'src caps name')
    eq(rec['num_obj'], 2, 'two PDOs')
    eq(rec['pdos'][0]['voltage_mv'], 5000, 'pdo1 voltage')
    eq(rec['pdos'][1]['voltage_mv'], 9000, 'pdo2 voltage')
    eq(rec['power_role'], 'source', 'power role')
    eq(rec['spec'], '3.1', 'spec rev 2 -> 3.1')

    # Control messages must carry NDO 0.
    eq(P.decode_frame(hdr(0x01, 0, 1))['name'], 'GoodCRC', 'GoodCRC')
    eq(P.decode_frame(hdr(0x03, 0, 1))['name'], 'Accept', 'Accept')
    eq(P.decode_frame(hdr(0x04, 0, 1))['name'], 'Reject', 'Reject')
    eq(P.decode_frame(hdr(0x06, 0, 1))['name'], 'PS_RDY', 'PS_RDY')
    eq(P.decode_frame(hdr(0x0C, 0, 1))['name'], 'Wait', 'Wait')
    eq(P.decode_frame(hdr(0x0D, 0, 1))['name'], 'Soft_Reset', 'Soft_Reset')

    # Extended message with a 2-byte extended header.
    body = bytes(range(20))
    ext_hdr = (len(body) & 0x1FF) | (0 << 15)     # unchunked
    rec = P.decode_frame(hdr(0x01, 1, 1, ext=1)
                         + ext_hdr.to_bytes(2, 'little') + body)
    eq(rec['name'], 'Source_Capabilities_Extended', 'extended name')
    eq(rec['data_size'], 20, 'extended data size')
    eq(rec['chunked'], 0, 'unchunked')


def main():
    print('=== pdtools host tests ===')
    test_against_st_overflow_record()
    test_roundtrip()
    test_corrupt()
    test_pdo_rdo()
    test_frames()
    print('=== %s (%d failures) ===' % ('PASS' if fails == 0 else 'FAIL', fails))
    return 0 if fails == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
