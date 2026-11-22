"""Minimal STFS (XBLA/LIVE/PIRS/CON) extractor. Usage: stfs_extract.py <package> [outdir]  (no outdir = list only)."""
import struct, sys, os

B = 0x1000
f = open(sys.argv[1], 'rb')
out = sys.argv[2] if len(sys.argv) > 2 else None
hdr = f.read(0x1000)
assert hdr[:4] in (b'LIVE', b'PIRS', b'CON '), hdr[:4]
header_size = struct.unpack('>I', hdr[0x340:0x344])[0]
first = ((header_size + 0xFFF) & ~0xFFF) + 0x1000  # hash tables PRECEDE their 0xAA data blocks
vd = hdr[0x379:0x379 + 0x24]  # volume descriptor
sep = vd[2]
ft_blocks = struct.unpack('<H', vd[3:5])[0]
ft_start = int.from_bytes(vd[5:8], 'little')
shift = 0 if sep & 1 else 1  # read-only packages: 1 hash block per table, else 2

def off(n):
    adj = 0
    if n >= 0xAA: adj += ((n // 0xAA) + 1) << shift
    if n >= 0x70E4: adj += ((n // 0x70E4) + 1) << shift
    return first + (n + adj) * B

def nxt(n):  # next block via L0 hash table (entry: 20B hash, 1B status, 3B next BE)
    t = off(n - n % 0xAA) - B  # table right before the group's first block
    f.seek(t + (n % 0xAA) * 0x18 + 21)
    return int.from_bytes(f.read(3), 'big')

def read_blocks(start, count, consecutive, size):
    data, n = bytearray(), start
    for i in range(count):
        f.seek(off(n)); data += f.read(B)
        n = n + 1 if consecutive else nxt(n)
    return bytes(data[:size]) if size is not None else bytes(data)

tbl = read_blocks(ft_start, ft_blocks, False, None)
ents = []
for i in range(0, len(tbl), 0x40):
    e = tbl[i:i + 0x40]
    ln = e[0x28] & 0x3F
    if ln == 0: break
    ents.append(dict(name=e[:ln].decode('latin1'), cons=bool(e[0x28] & 0x40), dir=bool(e[0x28] & 0x80),
                     blocks=int.from_bytes(e[0x29:0x2C], 'little'), start=int.from_bytes(e[0x2F:0x32], 'little'),
                     parent=struct.unpack('>h', e[0x32:0x34])[0], size=struct.unpack('>I', e[0x34:0x38])[0]))

def path(i):
    p = []
    while i != -1:
        p.append(ents[i]['name']); i = ents[i]['parent']
    return os.path.join(*reversed(p))

for i, e in enumerate(ents):
    p = path(i)
    print(('D ' if e['dir'] else 'F ') + p, e['size'])
    if out and not e['dir']:
        fp = os.path.join(out, p); os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, 'wb') as o:
            n, left = e['start'], e['size']
            for _ in range(e['blocks']):
                f.seek(off(n)); o.write(f.read(min(B, left))); left -= B
                n = n + 1 if e['cons'] else nxt(n)
