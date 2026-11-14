#!/usr/bin/env python3
"""Where an app's icon is on the phone's home screen, by the app's name: prints "X Y".

  appslot.py NAME                      the real apps (apps/*)
  appslot.py NAME --tests "DIR DIR"    the same plus those app folders (the tests' misbehaving apps),
                                       in that order, as `make EXTRA_APPS="DIR DIR"` has them
  appslot.py NAME --drawer             where it is in the all-apps drawer instead (alphabetical, 4 columns
                                       of 168 x 184 px tiles from y 216)

So a test taps "Calculator" and adding an app can't move it: the apps are laid out the way the
shell does it (kernel/appreg.c, kernel/desktop.c):
  - the ramdisk lists apps/* (alphabetically by folder) then EXTRA_APPS, in that order;
  - they're sorted by "order" in kerns.json (100 if it has none), keeping the list's order for equal ones;
  - the shell's own Log app comes last;
  - the dock holds Files, Browser, Gallery and Settings (the ones there are), in that order;
  - the others fill the grid: 4 columns of 168 x 184 px tiles from y 352, centred on a 720 px screen.
The 720x1280 phone screen is assumed, like the rest of the tests.
"""
import glob
import json
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
DOCK = ["Files", "Browser", "Gallery", "Settings"]
W = 720


def manifest(folder):
    with open(os.path.join(folder, "kerns.json"), encoding="utf-8") as f:
        j = json.load(f)
    return j["name"], int(j.get("order", 100))


def main():
    args = sys.argv[1:]
    drawer = "--drawer" in args
    args = [a for a in args if a != "--drawer"]
    if not args:
        raise SystemExit(__doc__)
    name = args[0]
    extra = []
    if len(args) >= 3 and args[1] == "--tests":
        extra = args[2].split()
    folders = sorted(glob.glob(os.path.join(ROOT, "apps", "*"))) + [os.path.join(ROOT, e) if not os.path.isabs(e) else e for e in extra]
    apps = []
    for d in folders:
        if os.path.exists(os.path.join(d, "kerns.json")):
            apps.append(manifest(d))
    apps.sort(key=lambda a: a[1])               # stable: equal orders keep the list's order
    names = [a[0] for a in apps] + ["Log"]
    if name not in names:
        sys.stderr.write("no app called %r here (%s)\n" % (name, ", ".join(names)))
        raise SystemExit(1)
    if drawer:
        k = sorted(names, key=str.lower).index(name)
        print((W - 4 * 168) // 2 + 84 + (k % 4) * 168, 216 + (k // 4) * 184)
        return
    dock = [n for n in DOCK if n in names]
    if name in dock:
        slot = dock.index(name)
        w = 678 // len(dock)                    # the dock is 678 px wide (20 px in each side), shared equally
        print(20 + slot * w + w // 2, 1168)     # three: 133, 359, 585; four: 109, 279, 449, 619
        return
    grid = [n for n in names if n not in dock]
    g = grid.index(name)
    print((W - 4 * 168) // 2 + 84 + (g % 4) * 168, 352 + 72 + (g // 4) * 184)


if __name__ == "__main__":
    main()
