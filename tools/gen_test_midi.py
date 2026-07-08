#!/usr/bin/env python3
"""Generates test-midi/flowr_test.mid: a tiny format-1 MIDI file with a
melody track, a bass/harmony track, and a percussion hit, used to
exercise LibreSCC's built-in parser and synth without any external
MIDI tooling or libraries."""
import struct
import os

def vlq(n):
    bytes_out = [n & 0x7F]
    n >>= 7
    while n:
        bytes_out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(bytes_out))

def track_chunk(events):
    data = b"".join(events)
    return b"MTrk" + struct.pack(">I", len(data)) + data

def note_on(delta, chan, note, vel):
    return vlq(delta) + bytes([0x90 | chan, note, vel])

def note_off(delta, chan, note, vel=0):
    return vlq(delta) + bytes([0x80 | chan, note, vel])

def program_change(delta, chan, prog):
    return vlq(delta) + bytes([0xC0 | chan, prog])

def tempo_meta(delta, bpm):
    us = int(60_000_000 / bpm)
    return vlq(delta) + bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", us)[1:]

def end_of_track(delta=0):
    return vlq(delta) + bytes([0xFF, 0x2F, 0x00])

DIVISION = 480  # ticks per quarter note

# Track 0: tempo + melody (channel 0, program 80 -> Synth Lead family -> Lead Square timbre)
melody = []
melody.append(tempo_meta(0, 128))
melody.append(program_change(0, 0, 80))
scale = [60, 62, 64, 65, 67, 69, 71, 72]  # C major scale
t = []
delta = 0
for i, note in enumerate(scale + list(reversed(scale))):
    t.append(note_on(delta, 0, note, 100))
    t.append(note_off(DIVISION // 2, 0, note))
    delta = 0
melody += t
melody.append(end_of_track())

# Track 1: bass/harmony (channel 1, program 33 -> Bass family -> Bass Triangle timbre)
bass = []
bass.append(program_change(0, 1, 33))
bassline = [36, 36, 41, 41, 43, 43, 36, 36]
b = []
delta = 0
for note in bassline:
    b.append(note_on(delta, 1, note, 90))
    b.append(note_off(DIVISION * 2, 1, note))
    delta = 0
bass += b
bass.append(end_of_track())

# Track 2: percussion (channel 9, GM drum channel -> Noise Perc timbre regardless of program)
perc = []
p = []
delta = DIVISION  # start on beat 2
for i in range(8):
    p.append(note_on(delta, 9, 38, 110))  # acoustic snare
    p.append(note_off(60, 9, 38))
    delta = DIVISION - 60
perc += p
perc.append(end_of_track())

header = b"MThd" + struct.pack(">IHHH", 6, 1, 3, DIVISION)
data = header + track_chunk(melody) + track_chunk(bass) + track_chunk(perc)

out_path = os.path.join(os.path.dirname(__file__), "..", "test-midi", "flowr_test.mid")
with open(out_path, "wb") as f:
    f.write(data)
print(f"wrote {out_path} ({len(data)} bytes)")
