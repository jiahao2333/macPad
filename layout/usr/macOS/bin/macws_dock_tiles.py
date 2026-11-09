#!/var/jb/usr/bin/python3
"""Drop Dock application tiles whose bundle does not exist in the chroot.

Ventura's Dock resolves each persistent-apps tile once, when Dock starts.
In MacWS the stock default Dock template names apps that a given rootfs may
not contain (Keynote/Numbers/Pages) or that only appear later (Safari lives in
the App cryptex, which may be installed after the first session).  Such tiles
are drawn as "?" and clicking them never reaches LSOpenFromURLSpec, so no
launch request is produced at all (runtime-confirmed 2026-11-09, iPad14,5).

This helper runs before Dock is (re)loaded.  It reads the Dock domain through
cfprefsd (stock `defaults export`), removes only file tiles whose file URL
does not resolve inside the chroot, and writes the domain back with
`defaults import` only when something changed.  Tiles for apps that exist are
kept unchanged; Dock re-resolves them (including bundle identifiers/icons) on
its next start, which is how a later-installed app such as Safari heals.

Usage: macws_dock_tiles.py ROOTFS EXPORTED_PLIST OUTPUT_PLIST
Exit 0 = output written (changed), 3 = nothing to change, other = error.
"""
import os
import plistlib
import stat
import sys
from urllib.parse import unquote, urlparse

MAX_LINKS = 40


def resolve_in_root(root, path):
    """Return True if absolute chroot path exists, following symlinks
    relative to the chroot root (absolute link targets restart at root)."""
    parts = [p for p in path.split('/') if p]
    current = []
    hops = 0
    while parts:
        name = parts.pop(0)
        if name == '.':
            continue
        if name == '..':
            if current:
                current.pop()
            continue
        candidate = current + [name]
        host = os.path.join(root, *candidate)
        try:
            st = os.lstat(host)
        except OSError:
            return False
        if stat.S_ISLNK(st.st_mode):
            hops += 1
            if hops > MAX_LINKS:
                return False
            target = os.readlink(host)
            new_parts = [p for p in target.split('/') if p]
            if target.startswith('/'):
                current = []
            parts = new_parts + parts
            continue
        current = candidate
    return True


def tile_path(tile):
    data = tile.get('tile-data') or {}
    file_data = data.get('file-data') or {}
    url = file_data.get('_CFURLString')
    if not isinstance(url, str):
        return None
    kind = file_data.get('_CFURLStringType')
    if kind == 0 or url.startswith('/'):
        return url
    parsed = urlparse(url)
    if parsed.scheme != 'file':
        return None
    return unquote(parsed.path)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    root, source, output = argv[1:]
    with open(source, 'rb') as handle:
        domain = plistlib.load(handle)
    removed = []
    for key in ('persistent-apps', 'recent-apps'):
        tiles = domain.get(key)
        if not isinstance(tiles, list):
            continue
        kept = []
        for tile in tiles:
            if not isinstance(tile, dict) or tile.get('tile-type') != 'file-tile':
                kept.append(tile)
                continue
            path = tile_path(tile)
            if path and path.startswith('/') and not resolve_in_root(root, path):
                label = (tile.get('tile-data') or {}).get('file-label') or path
                removed.append('%s:%s' % (key, label))
                continue
            kept.append(tile)
        domain[key] = kept
    if not removed:
        print('dock-tiles unchanged')
        return 3
    domain['mod-count'] = int(domain.get('mod-count', 0) or 0) + 1
    with open(output, 'wb') as handle:
        plistlib.dump(domain, handle, fmt=plistlib.FMT_BINARY)
    print('dock-tiles removed=%d %s' % (len(removed), ' '.join(removed)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
