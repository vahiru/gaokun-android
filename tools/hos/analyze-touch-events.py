#!/usr/bin/env python3
"""Summarize getevent -lt MT protocol-B contacts; no device access.

Coordinates use [0, width-1] and [0, height-1]. A release SYN_REPORT ends a
contact but is not counted as an active frame. Missing coordinates stay null.
"""
import argparse
import json
import re
import sys

LINE = re.compile(r'^\s*\[\s*(\d+(?:\.\d+)?)\]\s+(?:(\S+):\s+)?'
                  r'(EV_\w+|[0-9a-fA-F]{4})\s+(\w+)\s+(-?[0-9a-fA-F]+)\s*$')
CODES = {'002f': 'ABS_MT_SLOT', '0039': 'ABS_MT_TRACKING_ID',
         '0035': 'ABS_MT_POSITION_X', '0036': 'ABS_MT_POSITION_Y'}


def analyze(lines, width=1600, height=2560):
    if width <= 0 or height <= 0:
        raise ValueError('dimensions must be positive')
    devices, contacts, bounds, warnings = {}, [], [], []
    event_count = reports = max_contacts = unparsed = 0

    def finish(contact, timestamp, reason):
        contact['end_time_s'] = timestamp
        contact['end_reason'] = reason
        contact['duration_s'] = round(max(0, timestamp-contact['start_time_s']), 9)

    for line_number, line in enumerate(lines, 1):
        match = LINE.match(line)
        if not match:
            if line.strip():
                unparsed += 1
            continue
        stamp, path, typ, code, raw = match.groups()
        timestamp = float(stamp)
        path = path or 'unspecified'
        event_count += 1
        device = devices.setdefault(path, {'selected': 0, 'slots': {}, 'ending': [],
                                          'last_report': None, 'dropped': False})
        typ = {'0000': 'EV_SYN', '0003': 'EV_ABS'}.get(typ, typ)
        code = CODES.get(code.lower(), code) if typ == 'EV_ABS' else code
        if typ == 'EV_SYN':
            code = {'0000': 'SYN_REPORT', '0003': 'SYN_DROPPED'}.get(code, code)
        value = int(raw, 16)
        if typ == 'EV_SYN' and code == 'SYN_DROPPED':
            warnings.append({'line': line_number, 'device': path, 'reason': 'SYN_DROPPED: contact continuity unknown'})
            for slot in device['slots'].values():
                if slot.get('contact'):
                    finish(slot.pop('contact'), timestamp, 'syn_dropped')
            for c in device['ending']:
                finish(c, timestamp, 'syn_dropped')
            device['ending'].clear()
            device['slots'].clear()
            device['dropped'] = True
            continue
        if device['dropped']:
            if typ == 'EV_SYN' and code == 'SYN_REPORT':
                device['dropped'] = False
            continue
        if typ == 'EV_ABS':
            if code == 'ABS_MT_SLOT':
                device['selected'] = value
                continue
            slot = device['slots'].setdefault(device['selected'], {'x': None, 'y': None})
            if code == 'ABS_MT_TRACKING_ID':
                if slot.get('contact'):
                    previous = slot.pop('contact')
                    previous['_pending_reason'] = 'released' if value in (-1, 0xffffffff) else 'replaced'
                    device['ending'].append(previous)
                if value not in (-1, 0xffffffff):
                    contact = {'index': len(contacts), 'device': path, 'slot': device['selected'],
                               'tracking_id': value, 'start_time_s': timestamp,
                               'end_time_s': None, 'end_reason': None, 'duration_s': 0,
                               'frames': 0, 'first_xy': None, 'last_xy': None,
                               'dx': None, 'dy': None, 'max_frame_interval_s': None,
                               'out_of_bounds_frames': 0, '_last_frame': None}
                    contacts.append(contact)
                    slot['contact'] = contact
            elif code in ('ABS_MT_POSITION_X', 'ABS_MT_POSITION_Y'):
                # Kernel event values are unsigned hex encodings of signed int32.
                slot['x' if code.endswith('_X') else 'y'] = value-0x100000000 if value >= 0x80000000 else value
        elif typ == 'EV_SYN' and code == 'SYN_REPORT':
            reports += 1
            if device['last_report'] is not None and timestamp < device['last_report']:
                warnings.append({'line': line_number, 'device': path, 'reason': 'timestamp moved backwards'})
            device['last_report'] = timestamp
            for c in device['ending']:
                finish(c, timestamp, c.pop('_pending_reason', 'released'))
            device['ending'].clear()
            for slot in device['slots'].values():
                c = slot.get('contact')
                if c is None:
                    continue
                c['frames'] += 1
                if c['_last_frame'] is not None:
                    gap = timestamp-c['_last_frame']
                    if gap >= 0:
                        c['max_frame_interval_s'] = round(max(c['max_frame_interval_s'] or 0, gap), 9)
                c['_last_frame'] = timestamp
                xy = [slot['x'], slot['y']]
                if None not in xy:
                    if c['first_xy'] is None:
                        c['first_xy'] = xy
                    c['last_xy'] = xy
                if ((xy[0] is not None and not 0 <= xy[0] < width) or
                        (xy[1] is not None and not 0 <= xy[1] < height)):
                    c['out_of_bounds_frames'] += 1
                    bounds.append({'contact': c['index'], 'time_s': timestamp, 'xy': xy})
            # Maximum across device streams at this report, not a sum of their maxima.
            active = sum(bool(s.get('contact')) for d in devices.values() for s in d['slots'].values())
            max_contacts = max(max_contacts, active)
    for device in devices.values():
        for c in device['ending']:
            finish(c, c['_last_frame'] or c['start_time_s'], 'incomplete_release_no_syn')
        for slot in device['slots'].values():
            if slot.get('contact'):
                c = slot['contact']
                finish(c, c['_last_frame'] or c['start_time_s'], 'capture_ended_active')
    for c in contacts:
        if c['first_xy'] is not None:
            c['dx'], c['dy'] = [b-a for a, b in zip(c['first_xy'], c['last_xy'])]
        c.pop('_last_frame', None)
        c.pop('_pending_reason', None)
    return {'status': 'no_events' if event_count == 0 else ('contacts_observed' if contacts else 'no_touch_contacts'),
            'device_failure_inferred': False, 'width': width, 'height': height,
            'parsed_events': event_count, 'unparsed_lines': unparsed, 'syn_reports': reports,
            'max_simultaneous_contacts': max_contacts, 'contacts': contacts,
            'out_of_bounds': bounds, 'warnings': warnings}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', nargs='?', default='-', help='getevent text file, or - for stdin')
    p.add_argument('--width', type=int, default=1600)
    p.add_argument('--height', type=int, default=2560)
    a = p.parse_args()
    if a.width <= 0 or a.height <= 0:
        p.error('dimensions must be positive')
    if a.input == '-':
        result = analyze(sys.stdin, a.width, a.height)
    else:
        with open(a.input) as stream:
            result = analyze(stream, a.width, a.height)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
