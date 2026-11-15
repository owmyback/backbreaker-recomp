#!/usr/bin/env python3
"""Per-second fps and dip report from a runtime run log (issue #78).

Usage:
    tools/fps_report.py LOG [--threshold N] [--from S --to S]
    tools/fps_report.py LOG --compare OTHER.log [--threshold N] [--from S --to S]

The runtime prints one `[rt] [gpu] N fps, ...` status line per second, so a run log is already
a second by second frame trace; this turns it into a report. No game files and no game code
are involved - it reads a text log.

`--threshold` (default 25) picks which seconds count as dips. `--from`/`--to` are inclusive
second numbers, counted from the first fps line (that one is t=0). `--compare` lines two logs
up second by second.
"""
import argparse
import re
import sys
from collections import Counter

# The status line, as printed by runtime/gpu/command_processor.cpp. Deliberately stops before
# the front buffer, so a change to what comes after it cannot break the report.
FPS_RE = re.compile(r'\[gpu\] (\d+) fps, (\d+) draws/s, (\d+) frames total')

# Everything else on the log is a candidate cause for the second it landed in.
FIRST_USE_RE = re.compile(r'\[render\] first use: (.*)')
PIPELINE_RE = re.compile(r'\[render\] pipeline (\d+) (0x[0-9a-f]+): (\S+)')
RESOLVE_RE = re.compile(r'\[render\] resolve destination ([0-9A-Fa-f]+): (\d+)x(\d+)')
# --screenshot / --screenshot-at log from the renderer, F12 from the window's presenter.
SCREENSHOT_RE = re.compile(r'\[(?:render|video)\] screenshot (\S+)')
CAPTURE_RE = re.compile(r'\[capture\] recording frame (\d+) into (\S+)')

# The "first use" line is a comma separated list; each part is a count, what it counts, and
# optionally how long that part took.
USE_PART_RE = re.compile(r'(\d+) ([a-z ]+?)(?: \((\d+) ms\))?$')

OVERHEAD = ('screenshot', 'capture')  # these cost frames themselves: not the game's fault
GROUPED = ('pipeline', 'resolve')     # often hundreds in one second: reported as a count


class Second:
    def __init__(self, t, fps, draws, frames_total):
        self.t, self.fps, self.draws, self.frames_total = t, fps, draws, frames_total
        self.events = []

    @property
    def overhead(self):
        return any(e[0] in OVERHEAD for e in self.events)


def parse(path):
    """Seconds in order, each with the events logged during it.

    The vblank thread prints a second's status line once that second is over, while the
    renderer logs its work as it happens (`first use` at the swap that ends the frame doing
    it). So what is logged between two status lines happened during the second the second
    line reports: in a real log the shaderc compile and the resolve destinations come right
    before the `9 fps` line of the dip they caused. One exception: a frame that stalls for
    longer than a second (the 1.8 s side-select compile before the shader cache) logs its
    `first use` at the swap that ends it, so it shows next to the second after the `0 fps` one.
    Lines after the last status line belong to a second that never closed and are dropped.
    """
    out, pending = [], []
    with open(path, errors='replace') as f:
        for line in f:
            m = FPS_RE.search(line)
            if m:
                s = Second(len(out), int(m.group(1)), int(m.group(2)), int(m.group(3)))
                s.events, pending = pending, []
                out.append(s)
                continue
            ev = classify(line.rstrip())
            if ev:
                pending.append(ev)
    return out


def classify(line):
    """(kind, what to print, what to count it as) for a line worth reporting, else None."""
    for kind, re_ in (('pipeline', PIPELINE_RE), ('resolve', RESOLVE_RE),
                      ('screenshot', SCREENSHOT_RE), ('capture', CAPTURE_RE)):
        m = re_.search(line)
        if not m:
            continue
        g = m.groups()
        if kind == 'pipeline':
            return (kind, '', '')                      # the handle says nothing: just count them
        if kind == 'resolve':
            return (kind, '', f'{g[1]}x{g[2]}')
        if kind == 'screenshot':
            return (kind, f'screenshot {g[0]}', '')
        return (kind, f'capture frame {g[0]}', '')
    m = FIRST_USE_RE.search(line)
    if m:
        return ('first use', format_first_use(m.group(1)), '')
    return None


def describe(events, width=200):
    """What else was logged in a second, in one short string.

    A dip second can carry two hundred "resolve destination" lines and the count is the whole
    point of them, so the repeatable kinds collapse to a count plus the sizes seen there.
    """
    out, done = [], set()
    for i, (kind, text, _) in enumerate(events):
        if kind not in GROUPED:
            out.append(text)
            continue
        if kind in done:
            continue
        done.add(kind)
        same = [c for k, _, c in events[i:] if k == kind]
        label = 'new pipelines' if kind == 'pipeline' else 'new resolve destinations'
        sizes = Counter(c for c in same if c)
        cls = ', ' + ', '.join(f'{s} x{n}' for s, n in sizes.most_common(3)) if sizes else ''
        out.append(f'{label} {len(same)}{cls}')
    s = '; '.join(out)
    return s[:width] + ' ...' if len(s) > width else s


def format_first_use(text):
    """`40 shaders translated (2 ms), 13 from the shader cache` -> `shaders translated 40
    (2 ms), from the shader cache 13`

    Counts and their times move next to the words they belong to: with two numbers up front
    the eye cannot tell which time goes with which count.
    """
    parts = []
    for chunk in text.split(', '):
        m = USE_PART_RE.match(chunk)
        if not m:
            parts.append(chunk)
            continue
        n, what, ms = m.group(1), m.group(2).strip(), m.group(3)
        parts.append(f'{what} {n}' + (f' ({ms} ms)' if ms else ''))
    return 'first use: ' + ', '.join(parts)


def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


def summary(secs):
    """Header line: how much of the run fell below the dip line, and what it cost at worst."""
    fps = [s.fps for s in secs]
    lo = lambda n: sum(1 for f in fps if f < n)
    pct = lambda n: f'{100.0 * n / len(fps):.1f}%' if fps else '-'
    head = (f'seconds {len(fps)}  mean {mean(fps):.1f}  min {min(fps) if fps else "-"}  '
            f'below 25 {lo(25)} ({pct(lo(25))})  below 20 {lo(20)} ({pct(lo(20))})')
    n_over = sum(1 for s in secs if s.overhead)
    if n_over:
        clean = [s.fps for s in secs if not s.overhead]
        rest = f'({mean(clean):.1f} fps mean, {min(clean)} min, without them)' if clean \
            else '(every second does)'
        head += f'\n        seconds holding a screenshot or capture: {n_over} {rest}'
    if secs:
        head += f'\n        frames rendered {secs[-1].frames_total} (last status line)'
    return head


def report(path, secs, threshold, lo_t, hi_t):
    secs = [s for s in secs if lo_t <= s.t <= hi_t]
    if not secs:
        return secs
    print(f'== {path}')
    print(summary(secs))
    dips = [s for s in secs if s.fps < threshold]
    print(f'dips below {threshold}: {len(dips)} of {len(secs)} seconds')
    for s in dips:
        flag = '  [overhead]' if s.overhead else ''
        print(f'  t={s.t:<4} {s.fps:>3} fps {s.draws:>5} draws/s{flag}  '
              f'{describe(s.events) or "-"}')
    return secs


def compare(pa, a, pb, b, threshold):
    """Two logs side by side, for the seconds either of them dipped in.

    The frames-total difference over the run is the number a headless A/B run is after: same
    wall clock either way, so it is the frame-rate ratio.
    """
    print(f'== {pa} vs {pb}   ("-" = no status line that second)')
    print(f'   A: {summary_line(a, threshold)}\n   B: {summary_line(b, threshold)}')
    rows = [(a[i] if i < len(a) else None, b[i] if i < len(b) else None)
            for i in range(max(len(a), len(b)))]
    rows = [(x, y) for x, y in rows
            if (x and x.fps < threshold) or (y and y.fps < threshold)]
    print(f'  t      A      B     B-A   what was logged')
    for x, y in rows:
        t = x.t if x else y.t
        fa = f'{x.fps:>3} fps' if x else '  -  '
        fb = f'{y.fps:>3} fps' if y else '  -  '
        d = f'{y.fps - x.fps:+5}' if x and y else '    -'
        why = describe(list(x.events if x else []) + list(y.events if y else []), 150)
        print(f'  {t:<4} {fa} {fb} {d}   {why or "-"}')
    # Summed over the seconds compared rather than read off the last cumulative total, so a
    # --from window does not count the frames before it (without one the two are the same).
    na, nb = sum(s.fps for s in a), sum(s.fps for s in b)
    if na and nb:
        print(f'  frames total {na} vs {nb}: {100.0 * nb / na:.1f}%')


def summary_line(secs, threshold):
    fps = [s.fps for s in secs]
    return (f'{len(fps)} seconds  mean {mean(fps):.1f}  min {min(fps) if fps else "-"}  '
            f'below {threshold} {sum(1 for f in fps if f < threshold)}')


def main():
    ap = argparse.ArgumentParser(description='per-second fps and dip report from a run log')
    ap.add_argument('log')
    ap.add_argument('--compare', metavar='OTHER.log')
    ap.add_argument('--threshold', type=int, default=25)
    ap.add_argument('--from', dest='from_s', type=int, default=0)
    ap.add_argument('--to', dest='to_s', type=int, default=1 << 30)
    a = ap.parse_args()
    if a.from_s > a.to_s:
        sys.exit('--from is after --to')
    if a.compare:
        window = lambda secs: [s for s in secs if a.from_s <= s.t <= a.to_s]
        sa, sb = window(parse(a.log)), window(parse(a.compare))
        if not sa or not sb:
            sys.exit('no [gpu] fps status lines in that window')
        compare(a.log, sa, a.compare, sb, a.threshold)
    elif not report(a.log, parse(a.log), a.threshold, a.from_s, a.to_s):
        sys.exit('no [gpu] fps status lines in that window')


if __name__ == '__main__':
    main()
