"""Optional: pre-seed Safari's first-run onboarding state (field notes §61.7, §62).

Ventura Safari 16 opens a modal onboarding sheet on first launch. That sheet
is app UI, not part of the macOS service stack, so this is an optional
convenience step: it writes exactly one key, WBSOnboardingStatesDefaultsKeyV0.2,
into root's com.apple.Safari.plist in the rootfs and only when it is absent.
`defaults write` from the host does not work for this domain (the chroot
cfprefsd owns it), so the plist is edited directly; the macOS cfprefsd picks
the value up at its next start (GUI restart). A backup goes to
/var/mobile/macws-rollback/com.apple.Safari.plist.pre-onboarding-<ts>.

Run with the iOS interpreter:  /var/jb/usr/bin/python3 macws_safari_first_run.py [--check]
"""
import os
import plistlib
import shutil
import sys
import time

PLIST = "/var/mnt/rootfs/var/root/Library/Preferences/com.apple.Safari.plist"
ROLLBACK = "/var/mobile/macws-rollback"
KEY = "WBSOnboardingStatesDefaultsKeyV0.2"
# Values read back from the device after Safari had dismissed the sheet (r86).
VALUE = {
    "CustomizeStartPage": 3,
    "EnableCloudSync": 4,
    "EnableHighlights": 2,
    "ExtensionsDiscovery": 3,
    "SetDefaultBrowser": 2,
}


def main(argv):
    root_prefs = os.path.dirname(PLIST)
    if not os.path.isdir(os.path.dirname(root_prefs)):
        print("[INFO] Safari onboarding: rootfs root Library missing; skipped")
        return 0
    data, fmt = {}, plistlib.FMT_BINARY
    if os.path.exists(PLIST):
        with open(PLIST, "rb") as f:
            raw = f.read()
        fmt = plistlib.FMT_XML if raw.lstrip().startswith(b"<") else plistlib.FMT_BINARY
        data = plistlib.loads(raw)
    if KEY in data:
        print("[INFO] Safari onboarding state already present; unchanged")
        return 0
    if "--check" in argv:
        print("[INFO] Safari onboarding state absent")
        return 1
    if os.path.exists(PLIST):
        os.makedirs(ROLLBACK, exist_ok=True)
        backup = "%s/com.apple.Safari.plist.pre-onboarding-%s" % (
            ROLLBACK, time.strftime("%Y%m%d-%H%M%S"))
        shutil.copy2(PLIST, backup)
        print("[INFO] backup %s" % backup)
    else:
        os.makedirs(root_prefs, exist_ok=True)
    data[KEY] = VALUE
    tmp = PLIST + ".macws-new"
    with open(tmp, "wb") as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(tmp, 0, 0)
    os.chmod(tmp, 0o600)
    os.replace(tmp, PLIST)
    print("[INFO] Safari onboarding state written to %s" % PLIST)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
