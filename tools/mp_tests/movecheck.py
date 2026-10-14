#!/usr/bin/env python3
"""movecheck.py - evaluate WASD movement-feel scenarios S1..S9 from the host debug log.

Run:  python3 -I movecheck.py [--log LOG] [--marks MARKS] [--out OUT] [options]

Inputs
  host debug log lines (CATA_MP_TEST_TRACE=1), prefix `HH:MM:SS.mmm INFO : `:
    Multiplayer: trace step    id=<n> t=<ms> from=x,y to=x,y dx= dy= dur=<ms>
    Multiplayer: trace blocked id=<n> t=<ms> dx= dy=
    Multiplayer: trace slide   id=<n> t=<ms> wanted=dx,dy took=dx,dy
    Multiplayer: trace swing   id=<n> t=<ms> dx= dy= hit=<0|1>
  MOVE_MARKS.txt written by movetest.sh, one line per scenario:
    S1 start=<epoch ms> end=<epoch ms> nominal=<ms> settle_end=<epoch ms>
      start      = before the xdotool key chain was launched
      end        = right after the chain returned (== last key event, +few ms)
      nominal    = scripted duration of the chain (first key event .. last key event)
      settle_end = end of the post-scenario quiet period
  => first key event time t0 = end - nominal; all scenario times are relative to t0.

Measured quantities / assumptions (explicit):
  * Event windows use the LOG-LINE time (HH:MM:SS.mmm, same clock as `date`), from
    t0-150 ms to settle_end.  Marks (epoch ms) are converted to local time-of-day.
  * "step start" for comparison with key events = log-line time of the step line
    (assumes the line is logged when the step begins).
  * Gaps between steps use the trace `t` (game clock) if it is consistent with the
    log clock (offset spread <= 250 ms), otherwise log-line times.  start(n)=t(n).
  * "after release" means a step starting later than release + TOL_REL (default 100 ms).
  * Direction convention: dx=+1 east (d), dx=-1 west (a), dy=-1 north (w), dy=+1 south (s).
"""
import argparse
import copy
import os
import re
import sys
import time

DAY = 86400000


def circ(d):
    """circular difference in ms of day, result in [-12h, 12h)."""
    return (d + DAY // 2) % DAY - DAY // 2


LINE_RE = re.compile(r'^\s*(\d{1,2}):(\d{2}):(\d{2})\.(\d{1,3})')
TRACE_RE = re.compile(r'Multiplayer: trace (step|blocked|slide|swing)\b(.*)$')
KV_RE = re.compile(r'(\w+)=(\S+)')


def pair(s):
    a, b = s.split(',')
    return int(a), int(b)


class Ev:
    def __init__(self, kind, tod, f, raw):
        self.kind = kind
        self.tod = tod          # ms of day from log prefix
        self.f = f
        self.raw = raw
        self.id = f.get('id')
        self.t = f.get('t')
        self.dx = f.get('dx')
        self.dy = f.get('dy')
        self.dur = f.get('dur', 0)
        self.rel = 0            # ms relative to scenario t0 (log clock)
        self.g = 0              # time used for gap computation

    def d(self):
        return (self.dx, self.dy)

    def __str__(self):
        if self.kind == 'step':
            return 'step  rel=%+5d t=%s dur=%s d=(%s,%s) %s->%s' % (
                self.rel, self.t, self.dur, self.dx, self.dy,
                '%d,%d' % self.f['from'] if 'from' in self.f else '?',
                '%d,%d' % self.f['to'] if 'to' in self.f else '?')
        if self.kind == 'blocked':
            return 'blocked rel=%+5d t=%s d=(%s,%s)' % (self.rel, self.t, self.dx, self.dy)
        if self.kind == 'slide':
            return 'slide rel=%+5d t=%s wanted=%s took=%s' % (
                self.rel, self.t, self.f.get('wanted'), self.f.get('took'))
        return 'swing rel=%+5d t=%s d=(%s,%s) hit=%s' % (
            self.rel, self.t, self.dx, self.dy, self.f.get('hit'))


def parse_log(path):
    evs, malformed = [], 0
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            if 'Multiplayer: trace ' not in line:
                continue
            m = LINE_RE.match(line)
            tm = TRACE_RE.search(line)
            if not m or not tm:
                malformed += 1
                continue
            h, mi, s, frac = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
            tod = ((h * 60 + mi) * 60 + s) * 1000 + int(frac.ljust(3, '0'))
            f = {}
            try:
                for k, v in KV_RE.findall(tm.group(2)):
                    if k in ('from', 'to', 'wanted', 'took'):
                        f[k] = pair(v)
                    else:
                        f[k] = int(v)
                evs.append(Ev(tm.group(1), tod, f, line.rstrip('\n')))
            except ValueError:
                malformed += 1
    return evs, malformed


MARK_RE = re.compile(r'^(S\d+)\s+start=(\d+)\s+end=(\d+)\s+nominal=(\d+)\s+settle_end=(\d+)')


def epoch_to_tod(ep_ms, tzmin=None, utc=False):
    sec = ep_ms // 1000
    if tzmin is not None:
        lt = time.gmtime(sec + tzmin * 60)
    elif utc:
        lt = time.gmtime(sec)
    else:
        lt = time.localtime(sec)
    return ((lt.tm_hour * 60 + lt.tm_min) * 60 + lt.tm_sec) * 1000 + ep_ms % 1000


def parse_marks(path):
    marks = {}
    with open(path) as fh:
        for line in fh:
            m = MARK_RE.match(line.strip())
            if m:
                marks[m.group(1)] = dict(start=int(m.group(2)), end=int(m.group(3)),
                                         nominal=int(m.group(4)), settle_end=int(m.group(5)))
    return marks


DIRNAME = {(1, 0): 'E', (-1, 0): 'W', (0, -1): 'N', (0, 1): 'S',
           (1, -1): 'NE', (-1, -1): 'NW', (1, 1): 'SE', (-1, 1): 'SW'}


def dn(d):
    return DIRNAME.get(tuple(d), '(%s,%s)' % tuple(d))


def seq(steps):
    if not steps:
        return '-'
    out, last, n = [], None, 0
    for s in steps:
        if s.d() == last:
            n += 1
        else:
            if last is not None:
                out.append('%sx%d' % (dn(last), n))
            last, n = s.d(), 1
    out.append('%sx%d' % (dn(last), n))
    return ' '.join(out)


class Res:
    def __init__(self):
        self.fails, self.notes = [], []

    def fail(self, m):
        self.fails.append(m)

    def note(self, m):
        self.notes.append(m)

    @property
    def status(self):
        return 'FAIL' if self.fails else 'PASS'

    def reason(self):
        parts = self.fails + self.notes
        return '; '.join(parts) if parts else 'ok'


class Sc:
    """One scenario window."""
    def __init__(self, name, t0_tod, nominal, settle_rel, steps, others, args):
        self.name, self.t0, self.nominal, self.settle_rel = name, t0_tod, nominal, settle_rel
        self.steps, self.others, self.args = steps, others, args

    @property
    def lag(self):
        durs = sorted(s.dur for s in self.steps if s.dur)
        med = durs[len(durs) // 2] if durs else 0
        return max(self.args.lag, med + 50)


def gaps(sc, held_end):
    """gap(n) = start(n+1) - (start(n)+dur(n)) for pairs whose second step starts while keys held."""
    out = []
    for a, b in zip(sc.steps, sc.steps[1:]):
        if b.rel <= held_end:
            out.append((a, b, b.g - (a.g + a.dur)))
    return out


def after_release(sc, release, res, what='steps'):
    late = [s for s in sc.steps if s.rel > release + sc.args.tol_rel]
    if late:
        res.fail('%d %s start after release (+%dms): last at %+dms'
                 % (len(late), what, sc.args.tol_rel, late[-1].rel - release))
    elif sc.steps:
        res.note('last step start %+dms vs release, ends %+dms'
                 % (sc.steps[-1].rel - release, sc.steps[-1].rel + sc.steps[-1].dur - release))


def first_latency(sc, res):
    if sc.steps:
        res.note('first step +%dms after first key' % sc.steps[0].rel)


def check_segments(sc, segs, res):
    """segs = [(start, end, (dx,dy)|None, label)]; steps starting in [start+lag, end) must have dir."""
    lag = sc.lag
    for (a, b, want, label) in segs:
        cons = [s for s in sc.steps if a + lag <= s.rel < b - sc.args.slop]
        if not cons:
            res.fail('%s: no steps in [%d..%d)ms (after lag %dms)' % (label, a + lag, b, lag))
            continue
        bad = [s for s in cons if s.d() != want]
        if bad:
            res.fail('%s: %d/%d steps not %s (e.g. %s at %+dms)'
                     % (label, len(bad), len(cons), dn(want), dn(bad[0].d()), bad[0].rel))


# ---- scenario checks -------------------------------------------------------

def s1(sc):
    r = Res()
    if len(sc.steps) < 2:
        r.fail('only %d step(s), need >=2' % len(sc.steps))
    bad = [s for s in sc.steps if s.d() != (1, 0)]
    if bad:
        r.fail('%d step(s) not E (first: %s)' % (len(bad), dn(bad[0].d())))
    after_release(sc, 1200, r)
    first_latency(sc, r)
    r.note('n=%d' % len(sc.steps))
    return r


def s2(sc):
    r = Res()
    if len(sc.steps) != 1:
        r.fail('%d steps, expected exactly 1' % len(sc.steps))
    elif sc.steps[0].d() != (1, 0):
        r.fail('step is %s, expected E' % dn(sc.steps[0].d()))
    else:
        r.note('1 step E, dur=%dms, at %+dms' % (sc.steps[0].dur, sc.steps[0].rel))
    return r


def s3(sc):
    r = Res()
    if not sc.steps:
        r.fail('no steps')
        return r
    f = sc.steps[0]
    if f.d() != (1, -1):
        r.fail('first step is %s, expected NE diagonal' % dn(f.d()))
    after_release(sc, 1025, r)
    first_latency(sc, r)
    r.note('seq: ' + seq(sc.steps))
    return r


def s4(sc):
    r = Res()
    if not sc.steps:
        r.fail('no steps')
        return r
    f = sc.steps[0]
    if f.d() == (1, -1):
        r.note('INFO: 70ms chord started diagonal (NE)')
    elif f.d() in ((0, -1), (1, 0)):
        r.note('INFO: 70ms chord started cardinal %s (allowed)' % dn(f.d()))
    else:
        r.note('INFO: 70ms chord started %s (unexpected direction)' % dn(f.d()))
    r.note('seq: ' + seq(sc.steps))
    late = [s for s in sc.steps if s.rel > 1070 + sc.args.tol_rel]
    if late:
        r.note('INFO: %d step(s) after release' % len(late))
    return r


def s5(sc):
    r = Res()
    diag = [s for s in sc.steps if s.d() == (1, -1)]
    if not diag:
        r.fail('no diagonal steps (seq: %s)' % seq(sc.steps))
        return r
    i0 = sc.steps.index(diag[0])
    later_card = [s for s in sc.steps[i0 + 1:] if s.d() != (1, -1)]
    if later_card:
        c = later_card[0]
        r.fail('%d non-diagonal step(s) after diagonal start, first %s at %+dms (d released at 1025ms, w at 1065ms)'
               % (len(later_card), dn(c.d()), c.rel))
    after_release(sc, 1065, r)
    r.note('seq: ' + seq(sc.steps))
    return r


def s6(sc):
    r = Res()
    tol = sc.args.tol_rel
    check_segments(sc, [(0, 800, (-1, 0), 'W-1'),
                        (800, 1600, (1, 0), 'E(d last)'),
                        (1600, 2400 + tol, (-1, 0), 'W-2')], r)
    late = [s for s in sc.steps if s.rel > 2400 + tol]
    if late:
        r.fail('%d step(s) after release' % len(late))
    big = [(a, b, g) for a, b, g in gaps(sc, 2400) if g > sc.args.gap_max]
    if big:
        a, b, g = max(big, key=lambda x: x[2])
        r.fail('pause: gap %dms > %dms before step at %+dms' % (g, sc.args.gap_max, b.rel))
    r.note('seq: ' + seq(sc.steps))
    return r


def s7(sc):
    r = Res()
    tol = sc.args.tol_rel
    check_segments(sc, [(0, 1000, (1, 0), 'E'), (1000, 2000 + tol, (0, -1), 'N')], r)
    late = [s for s in sc.steps if s.rel > 2000 + tol]
    if late:
        r.fail('%d step(s) after release' % len(late))
    bad = [(a, b, g) for a, b, g in gaps(sc, 2000) if g > a.dur]
    if bad:
        a, b, g = max(bad, key=lambda x: x[2])
        r.fail('gap %dms > step dur %dms before step at %+dms' % (g, a.dur, b.rel))
    r.note('seq: ' + seq(sc.steps))
    return r


CHECKS = {
    'S1': (s1, 'hold d 1.2s'),
    'S2': (s2, 'tap d 60ms'),
    'S3': (s3, 'chord w+25ms+d'),
    'S4': (s4, 'chord w+70ms+d (info)'),
    'S5': (s5, 'w+d, release d then w'),
    'S6': (s6, 'opposing a/d'),
    'S7': (s7, 'direction change d->w'),
}
# release time (ms after t0) up to which keys are held (for the S8 gap check)
HELD_END = {'S1': 1200, 'S3': 1025, 'S4': 1070, 'S5': 1065, 'S6': 2400, 'S7': 2000}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', default=os.path.join(here, 'ud_host/config/debug.log'))
    ap.add_argument('--marks', default=os.path.join(here, 'MOVE_MARKS.txt'))
    ap.add_argument('--out', default=os.path.join(here, 'MOVE_RESULT.txt'))
    ap.add_argument('--id', type=int, default=None, help='trace id to evaluate (default: most steps)')
    ap.add_argument('--window-pad', type=int, default=150, help='ms before t0 included in a window')
    ap.add_argument('--tol-rel', type=int, default=100, help='ms after release before a step counts as late')
    ap.add_argument('--gap-max', type=int, default=60, help='max idle gap between steps while keys held (S6/S8)')
    ap.add_argument('--lag', type=int, default=150, help='min ms after a key change before the new direction is required')
    ap.add_argument('--slop', type=int, default=40, help='ms before a key change where the step direction is still ambiguous (timing error)')
    ap.add_argument('--tz-offset-min', type=int, default=None, help='tz offset of the log clock (default: local tz)')
    ap.add_argument('--utc', action='store_true', help='log clock is UTC')
    args = ap.parse_args()

    out = []

    def p(s=''):
        out.append(s)
        print(s)

    p('movecheck: log=%s marks=%s' % (args.log, args.marks))
    try:
        evs, malformed = parse_log(args.log)
    except OSError as e:
        p('ERROR cannot read log: %s' % e)
        return finish(args, out, 2)
    try:
        marks = parse_marks(args.marks)
    except OSError as e:
        p('ERROR cannot read marks: %s' % e)
        return finish(args, out, 2)

    kinds = {}
    for e in evs:
        kinds[e.kind] = kinds.get(e.kind, 0) + 1
    p('trace lines in log: %s (malformed %d); scenarios marked: %s'
      % (kinds or 'NONE (was CATA_MP_TEST_TRACE=1 set?)', malformed, ' '.join(sorted(marks)) or 'NONE'))

    # scenario windows
    wins = {}
    for name, mk in marks.items():
        t0_ep = max(mk['start'], mk['end'] - mk['nominal'])
        t0 = epoch_to_tod(t0_ep, args.tz_offset_min, args.utc)
        settle_rel = mk['settle_end'] - t0_ep
        wins[name] = (t0, mk['nominal'], settle_rel, mk['end'] - mk['start'])

    def in_window(e, w):
        rel = circ(e.tod - w[0])
        return -args.window_pad <= rel <= w[2]

    # choose id
    counts = {}
    for e in evs:
        if e.kind == 'step' and any(in_window(e, w) for w in wins.values()):
            counts[e.id] = counts.get(e.id, 0) + 1
    pid = args.id if args.id is not None else (max(counts, key=counts.get) if counts else None)
    p('step counts per id inside scenario windows: %s -> evaluating id=%s' % (counts or '{}', pid))

    # clock consistency (trace t vs log time)
    offs = [circ(e.tod - e.t) for e in evs if e.kind == 'step' and e.id == pid and e.t is not None
            and any(in_window(e, w) for w in wins.values())]
    use_t = bool(offs) and (max(circ(o - offs[0]) for o in offs) - min(circ(o - offs[0]) for o in offs) <= 250)
    if offs:
        spread = max(circ(o - offs[0]) for o in offs) - min(circ(o - offs[0]) for o in offs)
        p('clock: trace t vs log time offset spread %dms -> gaps measured with %s'
          % (spread, 'trace t (game clock)' if use_t else 'log-line times (t inconsistent)'))
    p('params: window_pad=%d tol_rel=%d gap_max=%d lag_min=%d slop=%d' % (args.window_pad, args.tol_rel, args.gap_max, args.lag, args.slop))
    p('')

    results, scs = {}, {}
    for name in sorted(CHECKS, key=lambda s: int(s[1:])):
        if name not in wins:
            results[name] = ('SKIP', 'no marks for scenario')
            continue
        t0, nominal, settle_rel, wall = wins[name]
        steps, others = [], []
        for e in evs:
            if e.id != pid or not in_window(e, wins[name]):
                continue
            e = copy.copy(e)
            e.rel = circ(e.tod - t0)
            (steps if e.kind == 'step' else others).append(e)
        steps.sort(key=lambda e: (e.rel, e.t or 0))
        others.sort(key=lambda e: e.rel)
        for s in steps:
            s.g = s.t if (use_t and s.t is not None) else s.rel
        sc = Sc(name, t0, nominal, settle_rel, steps, others, args)
        scs[name] = sc
        if not evs:
            results[name] = ('FAIL', 'no trace lines in log at all')
            continue
        r = CHECKS[name][0](sc)
        blk = [o for o in others if o.kind == 'blocked']
        sld = [o for o in others if o.kind == 'slide']
        if blk:
            r.note('WARN %d blocked event(s) in window (obstacle? result unreliable)' % len(blk))
        if sld:
            r.note('%d slide event(s)' % len(sld))
        if wall - nominal > 400:
            r.note('WARN key chain took %dms vs nominal %dms (timing less exact)' % (wall, nominal))
        results[name] = (r.status, r.reason())

    # S8: stutter check over scenarios with held keys
    worst, per, anyfail = None, [], False
    s8bad = []
    n_checked = 0
    for name, held in sorted(HELD_END.items()):
        sc = scs.get(name)
        if not sc:
            continue
        gl = gaps(sc, held)
        n_checked += 1
        if not gl:
            per.append('%s: no gaps' % name)
            continue
        a, b, g = max(gl, key=lambda x: x[2])
        per.append('%s max %dms' % (name, g))
        if g > args.gap_max:
            anyfail = True
            s8bad.append(name)
        if worst is None or g > worst[0]:
            worst = (g, name, b.rel)
    if not n_checked:
        results['S8'] = ('SKIP', 'no scenarios with data')
    elif anyfail:
        results['S8'] = ('FAIL', 'stutter: gap(n+1 start - n end) > %dms; worst %dms in %s at %+dms; %s'
                         % (args.gap_max, worst[0], worst[1], worst[2], ', '.join(per)))
    else:
        results['S8'] = ('PASS', 'all gaps <= %dms (worst %s); %s'
                         % (args.gap_max, ('%dms in %s' % (worst[0], worst[1])) if worst else 'n/a', ', '.join(per)))
    results['S9'] = ('SKIP', 'melee sectors: TODO (no monster helper yet)')

    order = ['S%d' % i for i in range(1, 10)]
    names = dict((k, v[1]) for k, v in CHECKS.items())
    names['S8'] = 'no stutter while held'
    names['S9'] = 'melee sectors'
    p('%-3s %-5s %-24s %s' % ('', 'RES', 'scenario', 'reason / measurement'))
    for n in order:
        st, why = results.get(n, ('SKIP', 'not evaluated'))
        p('%-3s %-5s %-24s %s' % (n, st, names.get(n, ''), why))

    failed = [n for n in order if results.get(n, ('',))[0] == 'FAIL']
    if failed:
        p('')
        p('---- raw events of failed scenarios (rel = ms after first key event, log clock) ----')
        for n in failed:
            sc = scs.get(n)
            targets = [n] if n != 'S8' else [k for k in s8bad if k not in failed]
            for tn in targets:
                sc = scs.get(tn)
                if not sc:
                    continue
                p('[%s] t0=%d nominal=%dms: %d steps, %d other events' % (tn, sc.t0, sc.nominal, len(sc.steps), len(sc.others)))
                for e in sorted(sc.steps + sc.others, key=lambda e: e.rel):
                    p('   ' + str(e))
    p('')
    npass = sum(1 for n in order if results.get(n, ('',))[0] == 'PASS')
    p('SUMMARY: %d PASS, %d FAIL, %d SKIP' % (npass, len(failed),
                                             sum(1 for n in order if results.get(n, ('',))[0] == 'SKIP')))
    return finish(args, out, 1 if failed else 0)


def finish(args, out, code):
    try:
        with open(args.out, 'w') as fh:
            fh.write('\n'.join(out) + '\n')
    except OSError as e:
        print('ERROR cannot write %s: %s' % (args.out, e))
    return code


if __name__ == '__main__':
    sys.exit(main())
