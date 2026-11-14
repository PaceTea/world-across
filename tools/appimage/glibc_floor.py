#!/usr/bin/env python3
"""glibc_floor.py FLOOR OUT.h [LIBRARY...] -- a header that holds a build to the C library of FLOOR.

A program linked on this machine asks for the newest version of every C library function it calls,
and so refuses to start on a distribution whose glibc is older than this one's, though nearly all
of those functions have been there for decades. The header made here, force-included into every
file (-include OUT.h), binds each function whose newest version is later than FLOOR to the latest
version it had at FLOOR instead: a .symver directive per function, which changes nothing for files
that do not call it. The few functions that did not exist at FLOOR at all are listed at its end;
tools/appimage/glibc_compat.c stands in for the ones the program and SDL call.

The libraries are libc.so.6 and libm.so.6, as the compiler finds them, unless named.

  tools/appimage/glibc_floor.py 2.34 build-appimage/glibc_floor.h
"""
import re
import subprocess
import sys


def version(text):
    return tuple(int(part) for part in text.split('_', 1)[1].split('.'))


def library(name):
    path = subprocess.run(['cc', '-print-file-name=' + name], capture_output=True, text=True,
                          check=True).stdout.strip()
    if path == name:
        sys.exit(f'glibc_floor: the compiler does not know where {name} is')
    return path


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    floor = tuple(int(part) for part in sys.argv[1].split('.'))
    out = sys.argv[2]
    libraries = sys.argv[3:] or [library('libc.so.6'), library('libm.so.6')]

    # objdump -T: "<address> g    DF .text  <size>  GLIBC_2.38   strlcpy", the version in
    # parentheses when it is not the default one.
    line_re = re.compile(r'^[0-9a-f]+\s.*\s(\()?(GLIBC_[0-9.]+)\)?\s+(\S+)$')
    versions, defaults = {}, {}
    for path in libraries:
        dump = subprocess.run(['objdump', '-T', path], capture_output=True, text=True,
                              check=True).stdout
        for line in dump.splitlines():
            if '*UND*' in line:
                continue
            match = line_re.match(line)
            if not match:
                continue
            hidden, ver, name = match.groups()
            if name.startswith('GLIBC_'):
                continue                         # the version's own definition, not a function
            versions.setdefault(name, set()).add(ver)
            if not hidden:
                defaults[name] = ver

    pins, missing = [], []
    for name in sorted(defaults):
        if version(defaults[name]) <= floor:
            continue
        older = [v for v in versions[name] if version(v) <= floor]
        if older:
            pins.append((name, max(older, key=version)))
        else:
            missing.append((name, defaults[name]))

    floor_text = '.'.join(str(part) for part in floor)
    with open(out, 'w') as f:
        f.write(f'/* Made by tools/appimage/glibc_floor.py: the C library functions whose newest\n'
                f'   version is later than glibc {floor_text}, bound to the version they had then. */\n')
        f.write('#ifndef ALBW_GLIBC_FLOOR_H\n#define ALBW_GLIBC_FLOOR_H\n')
        for name, ver in pins:
            f.write(f'__asm__(".symver {name},{name}@{ver}");\n')
        f.write(f'/* Not in glibc {floor_text} at all:\n')
        for name, ver in missing:
            f.write(f'     {name} ({ver})\n')
        f.write('*/\n#endif\n')
    print(f'glibc_floor: {len(pins)} functions bound to glibc {floor_text} or earlier, '
          f'{len(missing)} not there yet -> {out}')


if __name__ == '__main__':
    main()
