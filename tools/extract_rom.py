"""extract_rom.py <card.3ds> [outdir] -- the pieces of a decrypted card image the build needs.

Writes the exheader, the executable segment (code.bin, decompressed) and the rest of the ExeFS to
outdir, rom-extract/ by default, and prints the segment layout and the services the code declares.
"""
import struct, sys, os

MU = 0x200
if len(sys.argv) not in (2, 3):
    sys.exit(__doc__.strip().splitlines()[0])
rom = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) == 3 else 'rom-extract'
os.makedirs(outdir, exist_ok=True)

f = open(rom, 'rb')
f.seek(0x120); poff = struct.unpack('<I', f.read(4))[0] * MU

def at(off, n):
    f.seek(poff + off); return f.read(n)

ncch = at(0, 0x200)
# The product code names the release. The entry points play has found are one release's
# addresses, kept per release in entries/<product code>.txt.
product = ncch[0x150:0x160].rstrip(bytes(1)).decode('ascii', 'replace')
open(f'{outdir}/product_code.txt', 'w').write(product + '\n')
print(f'product code : {product}')
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
exh_size = u32(ncch, 0x180)
exefs_off, exefs_size = u32(ncch, 0x1A0) * MU, u32(ncch, 0x1A4) * MU

# ---- exheader (SCI 0x200 + ACI 0x200 + accessdesc 0x400) ----
exh = at(0x200, 0x800)
open(f'{outdir}/exheader.bin', 'wb').write(exh)

flags = exh[0x00D]
compressed = bool(flags & 1)
print(f'title        : {exh[0:8].rstrip(bytes(1)).decode()}')
print(f'sci flags    : 0x{flags:02X}  compress_exefs_code={compressed} sd_app={bool(flags&2)}')
print(f'remaster ver : {struct.unpack_from("<H", exh, 0x00E)[0]}')

segs = {}
for name, o in (('text', 0x010), ('rodata', 0x020), ('data', 0x030)):
    addr, pages, size = struct.unpack_from('<III', exh, o)
    segs[name] = (addr, pages, size)
    print(f'{name:<7}      : addr=0x{addr:08X} pages={pages} (0x{pages*0x1000:X})  size=0x{size:X}')
bss = u32(exh, 0x03C)
print(f'stack        : 0x{u32(exh, 0x01C):X}')
print(f'bss          : 0x{bss:X}')
print(f'savedata     : 0x{struct.unpack_from("<Q", exh, 0x1C0)[0]:X}')
print(f'jump id      : {struct.unpack_from("<Q", exh, 0x1C8)[0]:016X}')

deps = [struct.unpack_from('<Q', exh, 0x040 + i * 8)[0] for i in range(48)]
deps = [d for d in deps if d]
print(f'\ndependencies ({len(deps)}): ' + ' '.join(f'{d:016X}' for d in deps))

# ---- ACI: ARM11 local system caps ----
aci = 0x200
progid = struct.unpack_from('<Q', exh, aci + 0x000)[0]
core_ver = u32(exh, aci + 0x008)
f1, f2, f0, prio = exh[aci + 0x00C:aci + 0x010]
old3ds_mode = (f0 >> 4) & 0xF
n3ds_mode = f2 & 0xF
MODES = {0: 'Prod (64MB)', 1: 'Undefined', 2: 'Dev1 (96MB)', 3: 'Dev2 (80MB)', 4: 'Dev3 (72MB)', 5: 'Dev4 (32MB)'}
N3DS = {0: 'Legacy (same as O3DS)', 1: 'Prod (124MB)', 2: 'Dev1 (178MB)', 3: 'Dev2 (124MB)'}
print(f'\nprogram id   : {progid:016X}')
print(f'core version : {core_ver}')
print(f'o3ds memmode : {old3ds_mode} = {MODES.get(old3ds_mode, "?")}')
print(f'n3ds memmode : {n3ds_mode} = {N3DS.get(n3ds_mode, "?")}')
print(f'ideal cpu    : {f0 & 3}  affinity=0x{(f0>>2)&3:X}  prio={prio}  '
      f'l2cache={bool(f1&1)} cpu804mhz={bool(f1&2)}')

fs_access = struct.unpack_from('<Q', exh, aci + 0x230 + 0x18)[0] & 0x00FFFFFFFFFFFFFF
FS_BITS = ['CategorySysApplication', 'CategoryHardwareCheck', 'CategoryFilesystemTool',
           'Debug', 'TwlCardBackup', 'TwlNandData', 'Boss', 'DirectSdmc', 'Core',
           'CtrNandRo', 'CtrNandRw', 'CtrNandRoWrite', 'CategorySystemSettings',
           'CardBoard', 'ExportImportIvs', 'DirectSdmcWrite', 'SwitchCleanup',
           'SaveDataMove', 'Shop', 'Shell', 'CategoryHomeMenu', 'SeedSave']
print(f'fs access    : 0x{fs_access:X} = ' +
      ', '.join(n for i, n in enumerate(FS_BITS) if fs_access >> i & 1))

svc = []
for i in range(34):
    s = exh[aci + 0x250 + i * 8: aci + 0x250 + i * 8 + 8].rstrip(bytes(1))
    if s: svc.append(s.decode('ascii', 'replace'))
print(f'\nservice access control ({len(svc)}):')
for i in range(0, len(svc), 6):
    print('   ' + '  '.join(f'{s:<14}' for s in svc[i:i + 6]))

# ---- ARM11 kernel capabilities ----
print('\nkernel caps:')
syscalls = set()
for i in range(28):
    d = u32(exh, aci + 0x170 + i * 4)
    if d == 0xFFFFFFFF: continue
    if d >> 27 == 0b11110:      # syscall mask
        idx = (d >> 24) & 7
        for b in range(24):
            if d >> b & 1: syscalls.add(idx * 24 + b)
    elif d >> 25 == 0b1111110:  # kernel release version
        print(f'   kernel version : {(d>>8)&0xFF}.{d&0xFF}')
    elif d >> 23 == 0b111111100:
        print(f'   handle table   : {d & 0x3FFFFF}')
    elif d >> 20 == 0b111111110:
        fl = d & 0xFFFFF
        print(f'   kernel flags   : 0x{fl:05X} '
              f'(allow_debug={bool(fl&1)} force_debug={bool(fl&2)} '
              f'shared_page_writing={bool(fl&0x10)} privileged_priority={bool(fl&0x20)} '
              f'main_function_arg={bool(fl&0x40)} '
              f'memtype={(fl>>8)&0xF})')
    elif d >> 31 == 0 and d >> 20 == 0b111111111 - 1:
        pass
    elif (d >> 20) & 0xFFF in (0xFFE, 0xFFF) or d >> 31 == 0:
        print(f'   mapping/other  : 0x{d:08X}')
print(f'   svc access ({len(syscalls)} syscalls): ' +
      ' '.join(f'0x{s:02X}' for s in sorted(syscalls)))

arm9 = exh[aci + 0x1F0: aci + 0x200]
print(f'   arm9 access    : {arm9[:7].hex()} desc_ver={arm9[15]}')

# ---- ExeFS ----
ex = at(exefs_off, 0x200)
files = []
for i in range(10):
    nm = ex[i * 16:i * 16 + 8].rstrip(bytes(1))
    o, s = struct.unpack_from('<II', ex, i * 16 + 8)
    if nm: files.append((nm.decode(), o, s))
print('\nexefs:')
for nm, o, s in files:
    print(f'   {nm:<8} off=0x{o:X} size=0x{s:X} ({s/2**20:.2f} MiB)')
    data = at(exefs_off + 0x200 + o, s)
    open(f'{outdir}/{nm.lstrip(".")}.{"bin" if nm==".code" else "bin"}', 'wb').write(data)

# ---- decompress .code (backward LZ / BLZ) ----
def blz_decompress(data):
    size = len(data)
    top_bottom, orig_bottom = struct.unpack_from('<II', data, size - 8)
    comp_size = top_bottom & 0xFFFFFF
    hdr_size = top_bottom >> 24
    dec_size = size + orig_bottom
    out = bytearray(data) + bytearray(dec_size - size)
    comp_start = size - comp_size
    src = size - hdr_size
    dst = dec_size
    while src > comp_start and dst > comp_start:
        src -= 1
        control = out[src]
        for _ in range(8):
            if control & 0x80:
                src -= 2
                seg = (out[src] << 8) | out[src + 1]
                n = (seg >> 12) + 3
                off = (seg & 0xFFF) + 3
                for _ in range(n):
                    dst -= 1
                    out[dst] = out[dst + off]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            control = (control << 1) & 0xFF
            if src <= comp_start or dst <= comp_start:
                break
    return bytes(out[:dec_size])

code = open(f'{outdir}/code.bin', 'rb').read()
if compressed:
    dec = blz_decompress(code)
    expect = sum(segs[k][1] * 0x1000 for k in ('text', 'rodata', 'data'))
    print(f'\nblz: 0x{len(code):X} -> 0x{len(dec):X}  '
          f'(exheader expects 0x{expect:X}) match={len(dec)==expect}')
    open(f'{outdir}/code_dec.bin', 'wb').write(dec)
    print('first 8 words @0x100000: ' +
          ' '.join(f'{w:08X}' for w in struct.unpack_from('<8I', dec, 0)))
