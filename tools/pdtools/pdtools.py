#!/usr/bin/env python3
"""Offline USB-PD capture decoder, JSONL exporter and UCPD-Monitor .cpd writer.

Reads the frame dump produced by the firmware's `cap raw [n]` CLI command (or a
plain file of hex bytes, one frame per line) and re-decodes it on the host with
the same field positions the firmware uses, so a capture taken on the bench can
be analysed without the board attached.

Usage:
    pdtools.py decode  capture.txt
    pdtools.py jsonl   capture.txt -o capture.jsonl
    pdtools.py cpd     capture.txt -o capture.cpd
    pdtools.py summary capture.txt

The .cpd written here follows the CSV-ish column layout UCPD-Monitor imports:
one header line then one row per message.  It is a best-effort import format,
not a byte-exact clone of the vendor tool's file - see NOTES at the bottom.
"""
import argparse
import json
import sys

CONTROL = {
    0x01: 'GoodCRC', 0x02: 'GotoMin', 0x03: 'Accept', 0x04: 'Reject',
    0x05: 'Ping', 0x06: 'PS_RDY', 0x07: 'Get_Source_Cap',
    0x08: 'Get_Sink_Cap', 0x09: 'DR_Swap', 0x0A: 'PR_Swap',
    0x0B: 'VCONN_Swap', 0x0C: 'Wait', 0x0D: 'Soft_Reset',
    0x0F: 'Not_Supported', 0x10: 'Get_Source_Cap_Extended',
    0x11: 'Get_Status', 0x13: 'FR_Swap',
}

DATA = {
    0x01: 'Source_Capabilities', 0x02: 'Request', 0x03: 'BIST',
    0x04: 'Sink_Capabilities', 0x05: 'Battery_Status', 0x06: 'Alert',
    0x07: 'Get_Country_Info', 0x08: 'Enter_USB', 0x09: 'EPR_Request',
    0x0A: 'EPR_Mode', 0x0B: 'Source_Info', 0x0C: 'Revision',
    0x0F: 'Vendor_Defined',
}

EXTENDED = {
    0x01: 'Source_Capabilities_Extended', 0x02: 'Status',
    0x03: 'Get_Battery_Cap', 0x04: 'Get_Battery_Status',
    0x05: 'Battery_Capabilities', 0x06: 'Get_Manufacturer_Info',
    0x07: 'Manufacturer_Info', 0x08: 'Security_Request',
    0x09: 'Security_Response', 0x0A: 'Firmware_Update_Request',
    0x0B: 'Firmware_Update_Response', 0x0C: 'PPS_Status',
    0x0D: 'Country_Info', 0x0E: 'Country_Codes',
    0x0F: 'Sink_Capabilities_Extended', 0x10: 'Extended_Control',
    0x11: 'EPR_Source_Capabilities', 0x12: 'EPR_Sink_Capabilities',
}

SOP = ['SOP', "SOP'", "SOP''", "SOP'_Debug", "SOP''_Debug"]


def decode_header(b0, b1):
    """PD message header, per the field positions in app_dec.h."""
    h = b0 | (b1 << 8)
    return {
        'type': h & 0x1F,
        'data_role': (h >> 5) & 1,
        'spec_rev': (h >> 6) & 3,
        'power_role': (h >> 8) & 1,
        'msg_id': (h >> 9) & 7,
        'num_obj': (h >> 12) & 7,
        'extended': (h >> 15) & 1,
    }


def msg_name(hdr):
    if hdr['extended']:
        return EXTENDED.get(hdr['type'], 'EXTENDED_0x%02X' % hdr['type'])
    if hdr['num_obj'] == 0:
        return CONTROL.get(hdr['type'], 'CONTROL_0x%02X' % hdr['type'])
    return DATA.get(hdr['type'], 'DATA_0x%02X' % hdr['type'])


def pdo_fixed(p):
    return {'kind': 'fixed',
            'voltage_mv': ((p >> 10) & 0x3FF) * 50,
            'current_ma': (p & 0x3FF) * 10,
            'epr_capable': (p >> 23) & 1}


def pdo_pps(p):
    return {'kind': 'pps',
            'min_mv': ((p >> 8) & 0xFF) * 100,
            'max_mv': ((p >> 17) & 0xFF) * 100,
            'max_ma': (p & 0x7F) * 50,
            'power_limited': (p >> 27) & 1}


def decode_pdo(p):
    kind = (p >> 30) & 3
    if kind == 3:
        if ((p >> 28) & 3) == 0:
            return pdo_pps(p)
        return {'kind': 'avs', 'pdp_w': p & 0xFF,
                'min_mv': ((p >> 8) & 0xFF) * 100,
                'max_mv': ((p >> 17) & 0x1FF) * 100}
    if kind == 0:
        return pdo_fixed(p)
    return {'kind': 'battery' if kind == 1 else 'variable', 'raw': '0x%08X' % p}


def decode_frame(payload, ts_us=0, direction='rx', sop=0):
    if len(payload) < 2:
        return {'error': 'short frame', 'len': len(payload)}

    hdr = decode_header(payload[0], payload[1])
    rec = {'ts_us': ts_us, 'dir': direction, 'sop': SOP[sop] if sop < len(SOP) else str(sop),
           'name': msg_name(hdr), 'msg_id': hdr['msg_id'],
           'num_obj': hdr['num_obj'], 'len': len(payload)}
    rec.update(hdr)
    rec['data_role'] = 'DFP' if hdr['data_role'] else 'UFP'
    rec['power_role'] = 'source' if hdr['power_role'] else 'sink'
    rec['spec'] = '3.%d' % (hdr['spec_rev'] - 1) if hdr['spec_rev'] else '?'

    body = payload[2:]
    if hdr['extended']:
        if len(body) >= 2:
            ext = body[0] | (body[1] << 8)
            rec['data_size'] = ext & 0x1FF
            rec['chunked'] = (ext >> 15) & 1
            rec['chunk_num'] = (ext >> 11) & 0xF
            rec['payload'] = body[2:2 + rec['data_size']].hex()
    elif hdr['num_obj'] and len(body) >= hdr['num_obj'] * 4:
        objs = [int.from_bytes(body[i * 4:i * 4 + 4], 'little')
                for i in range(hdr['num_obj'])]
        rec['objects'] = ['0x%08X' % o for o in objs]
        if hdr['type'] in (0x01, 0x04) and not hdr['extended']:
            rec['pdos'] = [decode_pdo(o) for o in objs]
        elif hdr['type'] == 0x02:
            r = objs[0]
            rec['rdo'] = {'pos': (r >> 28) & 0xF, 'giveback': (r >> 25) & 1,
                          'unchunked': (r >> 26) & 1, 'usb_comm': (r >> 23) & 1,
                          'cap_mismatch': (r >> 22) & 1, 'epr_mode': (r >> 21) & 1,
                          'max_curr_ma': (r & 0x3FF) * 10,
                          'op_curr_ma': ((r >> 10) & 0x3FF) * 10}
    return rec


def read_frames(path):
    """Accept `cap raw` output, bare hex lines, or JSONL with a 'hex' field."""
    frames = []
    ts = 0
    with open(path, 'r', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('{'):
                try:
                    j = json.loads(line)
                except ValueError:
                    continue
                if 'hex' in j:
                    frames.append((bytes.fromhex(j['hex']), j.get('ts_us', ts),
                                   j.get('dir', 'rx'), j.get('sop', 0)))
                    ts += 1
                continue
            # tolerate "[12] RX 0x90013600 ..." style dumps
            tokens = line.replace(',', ' ').split()
            hexparts = [t for t in tokens
                        if len(t) >= 2 and len(t) % 2 == 0
                        and all(c in '0123456789abcdefABCDEF' for c in t)]
            if not hexparts:
                continue
            raw = b''.join(bytes.fromhex(t) for t in hexparts)
            frames.append((raw, ts, 'rx', 0))
            ts += 1
    return frames


def cmd_decode(args):
    for raw, ts, d, sop in read_frames(args.capture):
        rec = decode_frame(raw, ts, d, sop)
        print('%-22s %s' % (rec.get('name', '?'), json.dumps(rec, sort_keys=True)))
    return 0


def cmd_jsonl(args):
    out = open(args.out, 'w') if args.out else sys.stdout
    n = 0
    for raw, ts, d, sop in read_frames(args.capture):
        rec = decode_frame(raw, ts, d, sop)
        rec['raw'] = raw.hex()
        out.write(json.dumps(rec, sort_keys=True) + '\n')
        n += 1
    if args.out:
        out.close()
    print('wrote %d records' % n, file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# Binary .cpd (STM32CubeMonitor-UCPD / TRACER_EMB TLV) writer and reader.
#
# The format is defined by USBPD_TRACE_Add() in the ST middleware shipped with
# this project (Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c).
# One record is:
#
#   0xFD 0xFD 0xFD 0xFD     TLV_SOF x4
#   TAG                     ((PortNum + 1) << 5) | 0x12   (DEBUG_STACK_MESSAGE)
#   LEN_HI LEN_LO           big-endian, = Size + TRACE_SIZE_HEADER_TRACE (9)
#   TYPE                    TRACE_EVENT, 1 byte
#   TIME                    4 bytes, LITTLE-endian
#   PORTNUM                 1 byte
#   SOP                     1 byte
#   SIZE_HI SIZE_LO         big-endian payload length
#   PAYLOAD                 Size bytes
#   0xA5 0xA5 0xA5 0xA5     TLV_EOF x4
#
# The overflow record in USBPD_TRACE_Init() is the authoritative worked
# example: tag 0x32, length 0x0018 = 24 = 9 + 15, size 0x000F = 15.
# ---------------------------------------------------------------------------

TLV_SOF = 0xFD
TLV_EOF = 0xA5
DEBUG_STACK_MESSAGE = 0x12
TRACE_PORT_BIT_POSITION = 5
TRACE_SIZE_HEADER_TRACE = 9
TLV_HEADER_SIZE = 3
TLV_SOF_SIZE = 4
TLV_EOF_SIZE = 4

# TRACE_EVENT values, from usbpd_trace.h as used by the ST stack.
TRACE_EVENT = {
    0: 'TLV', 1: 'MSG_IN', 2: 'MSG_OUT', 3: 'CADEVENT', 4: 'PE_STATE',
    5: 'CAD_LOW', 6: 'DEBUG', 7: 'SRC', 8: 'SNK', 9: 'NOTIF', 10: 'POWER',
    11: 'TCPM', 12: 'PRL_STATE', 13: 'PRL_EVENT', 14: 'PHY_NOTFRWD',
    15: 'CPU', 16: 'TIMEOUT', 18: 'UCSI', 19: 'MSG_MSC',
}

# TRACE_EVENT used for a captured PD message, by direction.
EV_MSG_IN = 1
EV_MSG_OUT = 2


def cpd_encode_record(ev_type, port, sop, ts, payload):
    """Build one binary .cpd record.  Mirrors USBPD_TRACE_Add() byte for byte."""
    size = len(payload)
    tag = (((port + 1) & 0x07) << TRACE_PORT_BIT_POSITION) | DEBUG_STACK_MESSAGE
    length = size + TRACE_SIZE_HEADER_TRACE

    out = bytearray()
    out += bytes([TLV_SOF]) * TLV_SOF_SIZE
    out.append(tag & 0xFF)
    out.append((length >> 8) & 0xFF)          # big-endian
    out.append(length & 0xFF)
    out.append(ev_type & 0xFF)
    out += bytes([(ts >> (8 * i)) & 0xFF for i in range(4)])   # little-endian
    out.append(port & 0xFF)
    out.append(sop & 0xFF)
    out.append((size >> 8) & 0xFF)            # big-endian
    out.append(size & 0xFF)
    out += bytes(payload)
    out += bytes([TLV_EOF]) * TLV_EOF_SIZE
    return bytes(out)


def cpd_decode_stream(data):
    """Parse a binary .cpd stream back into records.

    Yields dicts, and reports framing problems rather than raising, so a
    truncated or corrupt file can still be partially recovered.
    """
    i = 0
    n = len(data)
    while i < n:
        # Scan for the SOF run.
        if data[i] != TLV_SOF:
            i += 1
            continue
        if data[i:i + TLV_SOF_SIZE] != bytes([TLV_SOF]) * TLV_SOF_SIZE:
            i += 1
            continue
        j = i + TLV_SOF_SIZE
        if j + TLV_HEADER_SIZE > n:
            yield {'error': 'truncated header', 'offset': i}
            return
        tag = data[j]
        length = (data[j + 1] << 8) | data[j + 2]
        body_start = j + TLV_HEADER_SIZE
        if body_start + length > n:
            yield {'error': 'truncated body', 'offset': i, 'length': length}
            return
        if length < TRACE_SIZE_HEADER_TRACE:
            yield {'error': 'length smaller than trace header',
                   'offset': i, 'length': length}
            i = body_start + length
            continue
        b = body_start
        ev = data[b]
        ts = int.from_bytes(data[b + 1:b + 5], 'little')
        port = data[b + 5]
        sop = data[b + 6]
        size = (data[b + 7] << 8) | data[b + 8]
        payload = data[b + 9:b + 9 + size]
        if len(payload) != size:
            yield {'error': 'payload shorter than size field',
                   'offset': i, 'size': size, 'have': len(payload)}
            return
        rec = {
            'tag': tag,
            'tag_port': (tag >> TRACE_PORT_BIT_POSITION) & 0x07,
            'tag_id': tag & 0x1F,
            'length': length,
            'event': ev,
            'event_name': TRACE_EVENT.get(ev, 'EVENT_%d' % ev),
            'ts': ts,
            'port': port,
            'sop': sop,
            'payload': payload,
        }
        yield rec
        i = body_start + length
        # Consume the EOF run if present; tolerate its absence.
        if data[i:i + TLV_EOF_SIZE] == bytes([TLV_EOF]) * TLV_EOF_SIZE:
            i += TLV_EOF_SIZE


def direction_for(direction):
    return EV_MSG_OUT if str(direction).lower() in ('tx', 'out', 'msg_out') \
        else EV_MSG_IN


def cmd_cpd(args):
    """Write a real binary .cpd file."""
    out = open(args.out, 'wb') if args.out else sys.stdout.buffer
    n = 0
    for raw, ts, d, sop in read_frames(args.capture):
        out.write(cpd_encode_record(direction_for(d), 0, sop, ts * 1000, raw))
        n += 1
    if args.out:
        out.close()
    print('wrote %d binary records to %s' % (n, args.out or '<stdout>'),
          file=sys.stderr)
    return 0


def cmd_cpdinfo(args):
    """Parse and summarise a binary .cpd file."""
    with open(args.capture, 'rb') as f:
        data = f.read()
    counts = {}
    bad = 0
    total = 0
    for rec in cpd_decode_stream(data):
        if 'error' in rec:
            print('  !! %s at offset %d' % (rec['error'], rec.get('offset', -1)))
            bad += 1
            continue
        total += 1
        counts[rec['event_name']] = counts.get(rec['event_name'], 0) + 1
    print('file        : %s (%d bytes)' % (args.capture, len(data)))
    print('records     : %d' % total)
    print('framing err : %d' % bad)
    for k in sorted(counts):
        print('  %-14s %d' % (k, counts[k]))
    return 0


def cmd_csv(args):
    cols = ['Timestamp', 'Direction', 'SOP', 'MessageType', 'MessageID',
            'NumberOfDataObjects', 'HexPayload']
    rows = []
    for raw, ts, d, sop in read_frames(args.capture):
        rec = decode_frame(raw, ts, d, sop)
        rows.append([str(rec['ts_us']), rec['dir'], rec['sop'], rec['name'],
                     str(rec['msg_id']), str(rec['num_obj']), raw.hex()])
    out = open(args.out, 'w') if args.out else sys.stdout
    out.write(','.join(cols) + '\n')
    for r in rows:
        out.write(','.join(r) + '\n')
    if args.out:
        out.close()
    print('wrote %d rows to %s' % (len(rows), args.out or '<stdout>'),
          file=sys.stderr)
    return 0


def cmd_summary(args):
    frames = read_frames(args.capture)
    by_name = {}
    by_sop = {}
    goodcrc = 0
    ids = set()
    for raw, ts, d, sop in frames:
        rec = decode_frame(raw, ts, d, sop)
        by_name[rec.get('name', '?')] = by_name.get(rec.get('name', '?'), 0) + 1
        by_sop[rec['sop']] = by_sop.get(rec['sop'], 0) + 1
        if rec.get('name') == 'GoodCRC':
            goodcrc += 1
        ids.add(rec['msg_id'])
    print('frames        : %d' % len(frames))
    print('GoodCRC       : %d' % goodcrc)
    print('message ids   : %s' % ','.join(str(i) for i in sorted(ids)))
    print('by SOP type   :')
    for k in sorted(by_sop):
        print('  %-14s %d' % (k, by_sop[k]))
    print('by message    :')
    for k in sorted(by_name, key=lambda x: -by_name[x]):
        print('  %-26s %d' % (k, by_name[k]))
    return 0


NOTES = """
.cpd format
-----------
`cpd` writes the binary TLV stream that USBPD_TRACE_Add() in the ST middleware
emits, which is what STM32CubeMonitor-UCPD records and what TRACER_EMB carries
over USART1.  `cpdinfo` parses such a file back.  `csv` is a separate, clearly
named delimited export and is not presented as .cpd.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=NOTES)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name, fn in (('decode', cmd_decode), ('jsonl', cmd_jsonl),
                     ('cpd', cmd_cpd), ('cpdinfo', cmd_cpdinfo),
                     ('csv', cmd_csv), ('summary', cmd_summary)):
        p = sub.add_parser(name)
        p.add_argument('capture')
        p.add_argument('-o', '--out')
        p.set_defaults(fn=fn)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
