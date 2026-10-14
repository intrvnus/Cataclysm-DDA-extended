#!/usr/bin/env python3
"""meleecheck.py - helper of melee_sector_test.sh (cursor-sector melee).

  meleecheck.py plan                       print "angle_deg exp_dx exp_dy" per planned click
  meleecheck.py point CX CY RADIUS_PX ANG  print the root pixel "x y" at ANG degrees (y down) around (CX, CY)
  meleecheck.py calib X Y DX DY TILEX TILEY  print the pixel of the player's tile centre
  meleecheck.py check LOG BODYID READYLINE PLAN OUT
        pair each host `trace cursor` line (the cursor vector the host used, sector it chose) with the next
        `trace swing` line of the body (dx dy hit) after line READYLINE of LOG; compare with
          (a) the sector computed here from the host's own cursor vector  (logic check)
          (b) the sector computed here from the nominal angle of the click  (end-to-end check)
        the target is a monster on the south-east tile (1,1); the neighbour-sector tolerance
        (12 degrees from the boundary) applies only towards that tile.
        Writes the report to OUT; exit 0 = PASS.

Angles are atan2(dy, dx) with y growing downward: 0 = east, 45 = south-east, 90 = south ...
"""
import math
import re
import sys

TX = [1, 1, 0, -1, -1, -1, 0, 1]
TY = [0, 1, 1, 1, 0, -1, -1, -1]
TARGET = (1, 1)
TOL = 12.0

# main sweep: the 8 octant centres, then boundary cases (tolerance towards the SE target, and away from it)
ANGLES = [0, 45, 90, 135, 180, 225, 270, 315, 20, 70, 5, 85, 340, 110]


def compass(octant):
    i = ((octant % 8) + 8) % 8
    return (TX[i], TY[i])


def expected(angle_deg):
    """sector for a cursor at angle_deg, a target on TARGET (mirrors cursor_sector_dir in fluid.cpp)"""
    a = math.radians(angle_deg)
    # atan2 returns (-180, 180]; fold the angle the same way; std::lround rounds half away from zero
    ang = math.atan2(math.sin(a), math.cos(a))
    octant = int(math.copysign(math.floor(abs(ang) / (math.pi / 4) + 0.5), ang))
    d = compass(octant)
    if d == TARGET:
        return d
    off = math.degrees(ang - octant * math.pi / 4)
    if abs(off) >= 22.5 - TOL:
        side = compass(octant + (1 if off > 0 else -1))
        if side == TARGET:
            return side
    return d


def cmd_plan():
    for a in ANGLES:
        e = expected(a)
        print("%d %d %d" % (a, e[0], e[1]))


def cmd_point(cx, cy, r, ang):
    a = math.radians(ang)
    print("%d %d" % (round(cx + r * math.cos(a)), round(cy + r * math.sin(a))))


def cmd_calib(x, y, dx, dy, tx, ty):
    print("%d %d" % (round(x - dx * tx), round(y - dy * ty)))


def cmd_check(log, body, ready, plan_file, out):
    plan = []
    for line in open(plan_file):
        p = line.split()
        if len(p) == 3:
            plan.append((float(p[0]), int(p[1]), int(p[2])))
    cur_re = re.compile(r'trace cursor id=(\d+) t=(\d+) cdx=(\S+) cdy=(\S+) sector=(-?\d+),(-?\d+)')
    sw_re = re.compile(r'trace swing id=(\d+) t=(\d+) dx=(-?\d+) dy=(-?\d+) hit=(\d)')
    pairs = []
    pending = None
    with open(log, errors='replace') as f:
        for n, line in enumerate(f, 1):
            if n <= ready:
                continue
            m = cur_re.search(line)
            if m and m.group(1) == body:
                pending = (float(m.group(3)), float(m.group(4)), (int(m.group(5)), int(m.group(6))))
                continue
            m = sw_re.search(line)
            if m and m.group(1) == body and pending is not None:
                pairs.append((pending, (int(m.group(3)), int(m.group(4))), int(m.group(5))))
                pending = None
    rep = []
    bad = 0
    rep.append("clicks planned %d, cursor+swing pairs in the host log %d" % (len(plan), len(pairs)))
    if len(pairs) != len(plan):
        rep.append("WARNING: the counts differ (missed or extra swings); pairing by order may be off")
    rep.append("%-6s %-9s %-14s %-9s %-9s %-4s %s" % ("angle", "nominal", "host cursor", "host sec", "swing", "hit", "verdict"))
    for i, (cur, sw, hit) in enumerate(pairs):
        cdx, cdy, sector = cur
        angle_seen = math.degrees(math.atan2(cdy, cdx))
        logic = expected(angle_seen)
        nominal = None
        nom_angle = None
        if i < len(plan):
            nom_angle, nx, ny = plan[i]
            nominal = (nx, ny)
        ok_logic = (sw == logic)
        ok_nom = (nominal is None or sw == nominal)
        # a swing into the target tile must report hit=1, any other none
        ok_hit = (hit == 1) == (sw == TARGET)
        ok = ok_logic and ok_nom and ok_hit
        if not ok:
            bad += 1
        rep.append("%-6s %-9s %-14s %-9s %-9s %-4d %s%s%s%s" % (
            "%.0f" % nom_angle if nom_angle is not None else "?",
            "%d,%d" % nominal if nominal else "?",
            "%.2f,%.2f(%.0f)" % (cdx, cdy, angle_seen),
            "%d,%d" % sector, "%d,%d" % sw, hit,
            "ok" if ok else "BAD",
            "" if ok_logic else " [swing != sector of the host's cursor %d,%d]" % logic,
            "" if ok_nom else " [swing != nominal sector %d,%d: cursor pixel off?]" % nominal,
            "" if ok_hit else " [hit flag wrong]"))
    verdict = "PASS" if (bad == 0 and len(pairs) >= len(plan)) else "FAIL"
    rep.append("bad: %d" % bad)
    rep.append(verdict)
    with open(out, 'a') as f:
        f.write("\n".join(rep) + "\n")
    return 0 if verdict == "PASS" else 1


if __name__ == '__main__':
    c = sys.argv[1]
    a = sys.argv[2:]
    if c == 'plan':
        cmd_plan()
    elif c == 'point':
        cmd_point(float(a[0]), float(a[1]), float(a[2]), float(a[3]))
    elif c == 'calib':
        cmd_calib(*[float(x) for x in a])
    elif c == 'check':
        sys.exit(cmd_check(a[0], a[1], int(a[2]), a[3], a[4]))
    else:
        sys.exit(2)
