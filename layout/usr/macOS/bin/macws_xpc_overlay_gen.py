#!/var/jb/usr/bin/python3
"""Generate launchd-managed overlay bundles for macOS XPC services.

Field notes §58/§59. For every <bundle>/XPCServices/<leaf>.xpc found in the
macOS rootfs, create (host namespace, under jbroot so xpcproxy can spawn it)

  /var/jb/usr/macOS/libexec/macws-xpc-overlay/<key>/<leaf>.xpc/
      Info.plist          stock plist, CFBundleExecutable=MacWSXPCProxy
      MacWSXPCProxy       hard link of the generic 0755 proxy
      MacWSXPCProxy.priv  hard link of the setuid 4755 sibling
      MacWSTarget  ->     stock executable (chroot path)
      MacWSSource  ->     stock bundle (chroot path, informational)

and the chroot-visible index /var/mnt/rootfs/usr/local/var/macws-xpc-overlay/
<key> ("<CFBundleIdentifier>\\t<chroot path>"). <key> = FNV-1a-64 (hex) of
the bundle's chroot realpath, the same string libmachook computes with
realpath(3) inside the chroot.

Usage (root, host side):
  macws_xpc_overlay_gen.py --all            scan the default roots
  macws_xpc_overlay_gen.py --requests       mirror bundles libmachook asked for
  macws_xpc_overlay_gen.py /chroot/path.xpc mirror specific bundles
  macws_xpc_overlay_gen.py --remove-all     delete every overlay + index
"""
import os
import plistlib
import shutil
import stat
import sys

ROOTFS = "/var/mnt/rootfs"
OVERLAY = "/var/jb/usr/macOS/libexec/macws-xpc-overlay"
INDEX = ROOTFS + "/usr/local/var/macws-xpc-overlay"
REQUESTS = ROOTFS + "/usr/local/var/macws-xpc-overlay-requests"
PROXY = "/var/jb/usr/macOS/libexec/MacWSXPCProxy"
PROXY_PRIV = PROXY + ".priv"
ROOTS = [
    "/System/Library/Frameworks",
    "/System/Library/PrivateFrameworks",
    "/System/Library/CoreServices",
    "/System/Applications",
    "/System/Volumes/Preboot/Cryptexes/App/System/Applications",
    "/Applications",
    "/Library/Frameworks",
]
EXE = "MacWSXPCProxy"


def fnv1a64(text):
    h = 0xcbf29ce484222325
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def chroot_realpath(path, depth=0):
    """realpath(3) of a chroot path, resolving symlinks inside ROOTFS."""
    if depth > 40:
        return None
    parts = [p for p in path.split("/") if p and p != "."]
    out = []
    i = 0
    while i < len(parts):
        part = parts[i]
        if part == "..":
            if out:
                out.pop()
            i += 1
            continue
        cand = "/" + "/".join(out + [part])
        host = ROOTFS + cand
        try:
            st = os.lstat(host)
        except OSError:
            return None
        if stat.S_ISLNK(st.st_mode):
            link = os.readlink(host)
            rest = "/".join(parts[i + 1:])
            if link.startswith("/"):
                if link == ROOTFS or link.startswith(ROOTFS + "/"):
                    link = link[len(ROOTFS):] or "/"
                nxt = link
            else:
                nxt = "/" + "/".join(out + [link])
            if rest:
                nxt = nxt.rstrip("/") + "/" + rest
            return chroot_realpath(nxt, depth + 1)
        out.append(part)
        i += 1
    return "/" + "/".join(out)


def find_bundles():
    seen = set()
    for root in ROOTS:
        host_root = ROOTFS + root
        if not os.path.isdir(host_root):
            continue
        for dirpath, dirnames, _ in os.walk(host_root, followlinks=False):
            if os.path.basename(dirpath) == "XPCServices":
                for d in list(dirnames):
                    if d.endswith(".xpc"):
                        p = dirpath[len(ROOTFS):] + "/" + d
                        if p not in seen:
                            seen.add(p)
                            yield p
                dirnames[:] = [d for d in dirnames if not d.endswith(".xpc")]
            # Do not descend into nested service bundles or huge resources.
            dirnames[:] = [d for d in dirnames
                           if d not in ("Resources", "_CodeSignature")]
        # XPCServices may itself be a symlink (Foo.framework/XPCServices ->
        # Versions/Current/XPCServices); os.walk does not follow it, and the
        # Versions/A copy is found directly.


def ensure_dir(path, mode=0o755):
    os.makedirs(path, exist_ok=True)
    os.chown(path, 0, 0)
    os.chmod(path, mode)


def link_or_copy(src, dst, mode):
    try:
        if os.path.lexists(dst):
            os.unlink(dst)
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)
        os.chown(dst, 0, 0)
        os.chmod(dst, mode)


def generate(chroot_bundle, verbose=True):
    real = chroot_realpath(chroot_bundle)
    if not real:
        return "skip-unresolved"
    host_bundle = ROOTFS + real
    plist_path = host_bundle + "/Contents/Info.plist"
    try:
        with open(plist_path, "rb") as f:
            info = plistlib.load(f)
    except Exception:
        return "skip-noplist"
    ident = info.get("CFBundleIdentifier")
    exe = info.get("CFBundleExecutable")
    if not ident or not exe:
        return "skip-nokeys"
    target = real + "/Contents/MacOS/" + exe
    if not os.path.exists(ROOTFS + target):
        return "skip-noexe"
    key = fnv1a64(real)
    leaf = os.path.basename(real)
    ensure_dir(OVERLAY)
    ensure_dir(OVERLAY + "/" + key)
    odir = OVERLAY + "/" + key + "/" + leaf
    ensure_dir(odir)
    info["CFBundleExecutable"] = EXE
    info["MacWSOverlaySource"] = real
    for k in ("CFBundleSupportedPlatforms", "LSMinimumSystemVersion",
              "DTPlatformName", "DTSDKName"):
        info.pop(k, None)
    tmp = odir + "/Info.plist.tmp"
    with open(tmp, "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_XML)
    os.chown(tmp, 0, 0)
    os.chmod(tmp, 0o644)
    os.replace(tmp, odir + "/Info.plist")
    link_or_copy(PROXY, odir + "/" + EXE, 0o755)
    link_or_copy(PROXY_PRIV, odir + "/" + EXE + ".priv", 0o4755)
    for name, value in (("MacWSTarget", target), ("MacWSSource", real)):
        p = odir + "/" + name
        if os.path.lexists(p):
            os.unlink(p)
        os.symlink(value, p)
        os.lchown(p, 0, 0)
    ensure_dir(INDEX)
    with open(INDEX + "/" + key, "w") as f:
        f.write("%s\t%s\n" % (ident, real))
    os.chmod(INDEX + "/" + key, 0o644)
    if verbose:
        rlt = (info.get("XPCService") or {}).get("RunLoopType", "-")
        print("OVERLAY %s %s id=%s runloop=%s" % (key, real, ident, rlt))
    return "ok"


def main(argv):
    if os.geteuid() != 0:
        print("must run as root", file=sys.stderr)
        return 2
    if "--remove-all" in argv:
        shutil.rmtree(OVERLAY, ignore_errors=True)
        shutil.rmtree(INDEX, ignore_errors=True)
        print("REMOVED")
        return 0
    for p in (PROXY, PROXY_PRIV):
        if not os.path.isfile(p):
            print("missing %s" % p, file=sys.stderr)
            return 3
    targets = []
    if "--all" in argv:
        targets.extend(find_bundles())
    if "--requests" in argv and os.path.isfile(REQUESTS):
        with open(REQUESTS) as f:
            targets.extend(l.strip() for l in f if l.strip().endswith(".xpc"))
    targets.extend(a for a in argv if a.startswith("/"))
    counts = {}
    quiet = "--quiet" in argv
    for t in targets:
        r = generate(t, verbose=not quiet)
        counts[r] = counts.get(r, 0) + 1
    if "--requests" in argv and os.path.isfile(REQUESTS):
        os.unlink(REQUESTS)
    print("SUMMARY " + " ".join("%s=%d" % kv for kv in sorted(counts.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
