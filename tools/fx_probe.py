# エフェクトの番地を確かめるための MIDI を組む。
# 出来た .mid を render.exe に通し、doc/effects.md のやり方で波形を測る。
#
#   python tools/fx_probe.py
#   build/render.exe roms fx_cho_off.mid fx_cho_off.wav 14

import struct
def vlq(n):
    out = bytearray([n & 0x7f]); n >>= 7
    while n:
        out.insert(0, (n & 0x7f) | 0x80); n >>= 7
    return bytes(out)
def sysex(d): return bytes([0xf0]) + vlq(len(d)) + bytes(d)
def xg(addr_and_data): return sysex([0x43,0x10,0x4c] + addr_and_data + [0xf7])

def build(path, sends, fx):
    ev = [(0, b'\xff\x51\x03' + struct.pack('>I', 500000)[1:]),
          (0, xg([0x00,0x00,0x7e,0x00]))]
    ev.append((480, b'\xb0\x5b' + bytes([sends[0]])))   # CC91 リバーブ
    ev.append((0,   b'\xb0\x5d' + bytes([sends[1]])))   # CC93 コーラス
    ev.append((0,   b'\xb0\x5e' + bytes([sends[2]])))   # CC94 バリエーション
    for a in fx:
        ev.append((0, xg(a)))
    ev.append((480, b'\x90\x3c\x64'))
    ev.append((480, b'\x80\x3c\x40'))
    ev.append((2880, b'\xff\x2f\x00'))
    trk = b''.join(vlq(d) + b for d, b in ev)
    open(path,'wb').write(b'MThd' + struct.pack('>IHHH',6,0,1,480) +
                          b'MTrk' + struct.pack('>I',len(trk)) + trk)

# コーラス: 02 01 20 が種別、という前提
build('fx_cho_off.mid',  (0,127,0), [[0x02,0x01,0x20, 0x00,0x00]])
build('fx_cho_on.mid',   (0,127,0), [[0x02,0x01,0x20, 0x41,0x02]])   # CHORUS3
# バリエーションをインサーションとして歪ませる
#   02 01 40 種別 / 02 01 5A 接続（0=インサーション） / 02 01 5B パート
build('fx_var_off.mid',  (0,0,127), [[0x02,0x01,0x40, 0x00,0x00],
                                     [0x02,0x01,0x5a, 0x00],
                                     [0x02,0x01,0x5b, 0x00]])
build('fx_var_dist.mid', (0,0,127), [[0x02,0x01,0x40, 0x49,0x00],   # DISTORTION
                                     [0x02,0x01,0x5a, 0x00],
                                     [0x02,0x01,0x5b, 0x00]])
build('fx_var_rot.mid',  (0,0,127), [[0x02,0x01,0x40, 0x45,0x00],   # ROTARY SPEAKER
                                     [0x02,0x01,0x5a, 0x00],
                                     [0x02,0x01,0x5b, 0x00]])
print('できた')
