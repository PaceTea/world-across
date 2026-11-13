import struct, sys

MU = 0x200

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

path = sys.argv[1]
f = open(path, 'rb')

hdr = f.read(0x200)
if hdr[0x100:0x104] != b'NCSD':
    print('not NCSD:', hdr[0x100:0x104]); sys.exit(1)

media_id = hdr[0x108:0x110][::-1].hex().upper()
print(f'NCSD  media id : {media_id}')
print(f'NCSD  size     : {u32(hdr,0x104)*MU/2**30:.2f} GiB')

parts = []
for i in range(8):
    off, size = struct.unpack_from('<II', hdr, 0x120 + i*8)
    if size:
        parts.append((i, off*MU, size*MU))
print('partitions     :', ', '.join(f'{i}@0x{o:X} ({s/2**20:.1f} MiB)' for i, o, s in parts))

for idx, poff, psize in parts:
    f.seek(poff)
    n = f.read(0x200)
    if n[0x100:0x104] != b'NCCH':
        print(f'\n[{idx}] no NCCH magic -> partition itself encrypted?')
        continue
    pid = n[0x118:0x120][::-1].hex().upper()
    prodcode = n[0x150:0x160].rstrip(b'\0').decode('ascii', 'replace')
    ver = struct.unpack_from('<H', n, 0x112)[0]
    flags = n[0x188:0x190]
    crypto_method = flags[3]
    content_type = flags[5]
    nocrypto = bool(flags[7] & 0x04)
    fixedkey = bool(flags[7] & 0x01)
    exh_size = u32(n, 0x180)
    plain_off, plain_size = u32(n, 0x190)*MU, u32(n, 0x194)*MU
    logo_off, logo_size = u32(n, 0x198)*MU, u32(n, 0x19C)*MU
    exefs_off, exefs_size = u32(n, 0x1A0)*MU, u32(n, 0x1A4)*MU
    romfs_off, romfs_size = u32(n, 0x1B0)*MU, u32(n, 0x1B4)*MU

    print(f'\n[{idx}] NCCH at 0x{poff:X}')
    print(f'     partition id : {pid}   product: {prodcode}  ver {ver}')
    print(f'     flags        : {flags.hex()}  crypto_method=0x{crypto_method:02X} '
          f'content_type=0x{content_type:02X}')
    print(f'     NoCrypto bit : {nocrypto}    FixedKey: {fixedkey}')
    print(f'     exheader     : 0x200 .. +0x{exh_size:X}')
    print(f'     plain region : 0x{plain_off:X} +0x{plain_size:X}')
    print(f'     logo         : 0x{logo_off:X} +0x{logo_size:X}')
    print(f'     exefs        : 0x{exefs_off:X} +0x{exefs_size:X} ({exefs_size/2**20:.1f} MiB)')
    print(f'     romfs        : 0x{romfs_off:X} +0x{romfs_size:X} ({romfs_size/2**20:.1f} MiB)')

    if plain_size:
        f.seek(poff + plain_off); print('     plain region :',
              f.read(min(plain_size, 0x100)).split(b'\0\0')[0].decode('ascii', 'replace').replace('\0', ' '))

    # exheader sanity: app title at 0x200+0x000 is 8 ascii bytes
    f.seek(poff + 0x200)
    exh = f.read(min(exh_size, 0x800))
    title = exh[0:8]
    readable = all(32 <= c < 127 or c == 0 for c in title)
    print(f'     exh title    : {title!r}  plausible={readable}')

    if exefs_size:
        f.seek(poff + exefs_off)
        ex = f.read(0x200)
        names = []
        for i in range(10):
            nm = ex[i*16:i*16+8].rstrip(b'\0')
            off_, sz = struct.unpack_from('<II', ex, i*16+8)
            if nm: names.append(f'{nm.decode("ascii","replace")}(0x{sz:X})')
        print(f'     exefs files  : {names if names else "UNREADABLE (encrypted)"}')

    if romfs_size:
        f.seek(poff + romfs_off)
        print(f'     romfs magic  : {f.read(4)!r} (expect b"IVFC")')
