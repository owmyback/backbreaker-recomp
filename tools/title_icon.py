#!/usr/bin/env python3
"""Write the title's own icon (a PNG) from its SPA/XDBF resource, for the AppImage's icon (#134).

Usage: title_icon.py default.pe out.png

The XEX carries an XDBF blob (layout: runtime/kernel/spa.h); section 2 holds PNG images and
id 0x8000 is the title image, the one the dashboard shows. Read from the player's own
extracted files; nothing of the game is committed. Exits 1 without an icon.
"""
import struct
import sys

TITLE_IMAGE = 0x8000


def title_image(pe):
    at = pe.find(b'XDBF')
    while at >= 0:
        try:
            _, _, entry_count, entry_used, free_count, _ = struct.unpack_from('>4sIIIII', pe, at)
            data = at + 24 + entry_count * 18 + free_count * 8
            for i in range(min(entry_used, entry_count)):
                section, ident, offset, size = struct.unpack_from('>HQII', pe, at + 24 + i * 18)
                if section == 2 and ident == TITLE_IMAGE:
                    png = pe[data + offset:data + offset + size]
                    if png.startswith(b'\x89PNG\r\n\x1a\n'):
                        return png
        except struct.error:
            pass
        at = pe.find(b'XDBF', at + 4)
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    png = title_image(open(sys.argv[1], 'rb').read())
    if png is None:
        print('title_icon: no title image in the XDBF resource', file=sys.stderr)
        sys.exit(1)
    open(sys.argv[2], 'wb').write(png)
    print(f'title_icon: {len(png)} bytes')


if __name__ == '__main__':
    main()
