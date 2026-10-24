#!/usr/bin/env python3
"""MineMic v2 test sender: explicit PC IPv4, optional six-digit code, synthetic PCM/WAV."""
import argparse
import hmac
import json
import math
import secrets
import socket
import struct
import time
import wave

HEADER = struct.Struct('!4sBBHIIIIHBBHH')
REFERENCE = struct.Struct('!4sBBBBIIQQIHHII')
RATE, FRAMES = 48000, 480

def control(stream, **fields):
    stream.write((json.dumps(dict(app='MineMic', v='2', **fields), separators=(',', ':')) + '\n').encode())
    stream.flush()
    expected = dict(connect='welcome', resume='welcome', ping='pong', clock='clock',
                    reference='reference', disconnect='disconnected')[fields['type']]
    for _ in range(8):
        line = stream.readline(1025)
        if len(line) > 1024 or not line.endswith(b'\n'):
            raise RuntimeError('Invalid PC response')
        reply = json.loads(line)
        if reply.get('app') != 'MineMic' or reply.get('v') != '2':
            raise RuntimeError('Incompatible PC protocol')
        if reply.get('type') in (expected, 'error'):
            return reply
    raise RuntimeError('Too many unrelated PC responses')

def decode_reference(data, key, session):
    if len(data) != 1040 or not hmac.compare_digest(data[-32:], hmac.digest(key, data[:-32], 'sha256')):
        raise ValueError('Invalid reference authentication/size')
    magic, version, channels, channel, flags, sid, sequence, position, micros, rate, frames, payload, epoch, reserved = REFERENCE.unpack(data[:48])
    if (magic, version, channels, flags, sid, rate, frames, payload, reserved) != (b'MREF', 1, 2, 0, session, RATE, FRAMES, 960, 0) or channel not in (0, 1):
        raise ValueError('Invalid reference header')
    return channel, sequence, position, micros, epoch, data[48:1008]

def packet(key, session, sequence, pcm):
    body = HEADER.pack(b'MMIC', 2, 3, 0, session, sequence & 0xffffffff,
                       sequence * FRAMES & 0xffffffff, RATE, FRAMES, 1, 1, len(pcm), 0) + pcm
    return body + hmac.digest(key, body, 'sha256')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--port', type=int, default=48721)
    parser.add_argument('--code', default='', help='Omit for manual IP connection')
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--frequency', type=float, default=440)
    parser.add_argument('--wav')
    parser.add_argument('--reference', action='store_true', help='Request negotiated PCM stereo reference (does not perform AEC)')
    args = parser.parse_args()
    if args.code and (len(args.code) != 6 or not args.code.isascii() or not args.code.isdigit()):
        parser.error('Pairing code must be exactly six ASCII digits')
    if not 0 < args.seconds < 86400 or not 0 < args.frequency < 24000:
        parser.error('Invalid duration/frequency')
    source = wave.open(args.wav, 'rb') if args.wav else None
    if source and (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getcomptype()) != (1, 2, RATE, 'NONE'):
        parser.error('Need 48 kHz mono PCM16 WAV')
    with socket.create_connection((args.host, args.port), timeout=3) as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        stream = tcp.makefile('rwb')
        client_id, nonce = secrets.token_hex(16), secrets.token_hex(16)
        welcome = control(stream, type='connect', mode='code' if args.code else 'manual', code=args.code,
                          clientId=client_id, nonce=nonce, name='Python test sender')
        if welcome.get('type') != 'welcome' or welcome.get('nonce') != nonce:
            raise RuntimeError('Pairing refused: ' + welcome.get('reason', 'invalid response'))
        key, session = bytes.fromhex(welcome['key']), int(welcome['session'])
        udp.connect((args.host, int(welcome['port'])))
        reference_packets = 0
        try:
            if args.reference:
                if 'ref-pcm-stereo-v1' not in welcome.get('caps', '').split(','):
                    raise RuntimeError('PC has no playback reference capability')
                clock = control(stream, type='clock', t1=str(time.monotonic_ns() // 1000))
                if clock.get('type') != 'clock': raise RuntimeError('Clock exchange refused')
                reply = control(stream, type='reference', enabled='1')
                if reply.get('state') not in ('ready', 'starting'): raise RuntimeError('Select an available PC speaker first')
                udp.setblocking(False)
            started = last_ping = time.monotonic(); sequence = 0
            while time.monotonic() - started < args.seconds:
                sequence = max(sequence, int((time.monotonic() - started) * 100))
                if source:
                    pcm = source.readframes(FRAMES)
                    if not pcm:
                        break
                    pcm = pcm.ljust(960, b'\0')
                else:
                    pcm = struct.pack('<480h', *[int(4000 * math.sin(2 * math.pi * args.frequency * (sequence * FRAMES + i) / RATE)) for i in range(FRAMES)])
                udp.send(packet(key, session, sequence, pcm)); sequence += 1
                if args.reference:
                    for _ in range(32):
                        try: data = udp.recv(1201)
                        except BlockingIOError: break
                        decode_reference(data, key, session); reference_packets += 1
                if time.monotonic() - last_ping >= 1:
                    control(stream, type='ping'); last_ping = time.monotonic()
                time.sleep(max(0, started + sequence / 100 - time.monotonic()))
        except KeyboardInterrupt:
            pass
        finally:
            reply = control(stream, type='disconnect', clientId=client_id, token=welcome['token'])
            if reply.get('type') != 'disconnected':
                raise RuntimeError('Disconnect not acknowledged')
            if source:
                source.close()
            stream.close()
        print('Paired, streamed, explicitly disconnected; PC pairing code preserved.')
        if args.reference: print('Authenticated reference packets:', reference_packets)
if __name__ == '__main__':
    main()
