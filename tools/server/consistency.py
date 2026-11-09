#!/usr/bin/env python3
"""How the search agrees with the static estimate at depth (docs/server_plan.md, §5).

  sample   - positions from the recorded games (ssbot positions: each team-turn start), every
             --every-th, without repeats, plus the 10 positions of v4; stratified by class
             (ball: White's / Black's / free; side to move; attack episode or not), at most --max.
  static   - per position: the leaf estimate as the search values it (as is and after EndTurn),
             the static estimate, its components (goal-static), the attack episode.
  depths   - V(d), fixed-depth searches d = 1..4 for all positions, 5 and 6 for subsets
             (--deep, --deeper); depth by depth, a depth that runs out of time stops the deeper ones.
  horizon  - the leaf of the principal line: from a position, the plan of a search of depth d is
             played, then the plan of a search of the remaining depth from there, and so on until
             d team turns are played (after a pass the outcome "ball through", as --then). At
             that leaf: the static estimate against searches of depth 1..K from it. The search
             chooses leaves where the estimate errs in its favour; this measures that error.
  depths --root-as-node - the same searches with the root as an inner node (cons_depths_node.csv):
             the difference is what the beam of an inner node loses for the side to move there.
  report   - per class and depth: bias and mean error of V(d) against the estimate, correlation
             (Pearson, Spearman), the change V(d+1) - V(d) (parity: the right to move), the
             horizon error by depth, the link of V(deepest) - estimate with the components.

Values are White's ("eval": + better for White). Rows are written as computed; a rerun
continues. Results: results/cons_*.csv, results/cons_report.txt.

  python3 tools/server/consistency.py sample --max 600
  python3 tools/server/consistency.py static
  python3 tools/server/consistency.py depths --depths 1,2,3,4,5,6 --deep 100 --deeper 30 --jobs 35
  python3 tools/server/consistency.py horizon --count 150 --line-depth 3 --check 1,2,3 --jobs 35
  python3 tools/server/consistency.py report
"""
import argparse, csv, glob, math, os, random, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sslib

POS = re.compile(r"position=(.*)$")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def path(name):
    return os.path.join(sslib.RESULTS, name)


def read(name):
    p = path(name)
    return list(csv.DictReader(open(p, newline="", encoding="utf-8"))) if os.path.exists(p) else []


def ball_class(position):
    """'W', 'B' (held by a player of that team) or 'free'."""
    m = re.search(r"@(\S+)", position)
    if not m: return "?"
    b = m.group(1)
    return b[0] if b[0] in "WB" and b[1:2].isdigit() else "free"


def pool(n, fn, jobs):
    with ThreadPoolExecutor(n) as ex: list(ex.map(fn, jobs))


def cmd_sample(a):
    t = sslib.Table(path("cons_positions.csv"), ["pid", "source", "position", "mover", "ball"], ["pid"])
    if t.rows():
        log("already sampled: %d positions" % len(t.rows())); return
    seen, found = set(), []
    for c in sslib.cases():
        seen.add(c["position"]); found.append(("v4 " + c["id"], c["position"]))
    games = sorted(glob.glob(os.path.join(a.games, "**", "*.ssn"), recursive=True))
    for g in games:
        r = subprocess.run([sslib.SSBOT, "positions", g], capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if POS.search(l)]
        for i, l in enumerate(lines):
            p = POS.search(l).group(1).strip()
            if i % a.every or p in seen: continue
            seen.add(p)
            found.append(("%s %s" % (os.path.relpath(g, a.games), l.split()[0]), p))
    log("%d games, %d distinct positions" % (len(games), len(found)))
    rng = random.Random(a.seed)
    v4 = [f for f in found if f[0].startswith("v4 ")]
    rest = [f for f in found if not f[0].startswith("v4 ")]
    groups = {}
    for f in rest: groups.setdefault((sslib.mover(f[1]), ball_class(f[1])), []).append(f)
    for g in groups.values(): rng.shuffle(g)
    chosen, k = list(v4), 0
    while len(chosen) < a.max and any(groups.values()):   # round robin over the classes
        for key in sorted(groups):
            if groups[key] and len(chosen) < a.max: chosen.append(groups[key].pop())
    for i, (source, p) in enumerate(chosen):
        t.add({"pid": "p%04d" % i, "source": source, "position": p, "mover": sslib.mover(p), "ball": ball_class(p)})
    counts = {}
    for r in t.rows(): counts[(r["mover"], r["ball"])] = counts.get((r["mover"], r["ball"]), 0) + 1
    log("sampled %d:" % len(chosen), ", ".join("%s to move, ball %s: %d" % (k[0], k[1], v) for k, v in sorted(counts.items())))


EST = re.compile(r"static ([-\d.]+), after EndTurn ([-\d.]+) \(for (White|Black)\)")
LEAF = re.compile(r"leaf ([-\d.]+), leaf after EndTurn ([-\d.]+)")
COMP = re.compile(r"(\w[\w-]*) ([-\d.]+)")


def static_of(position, bot):
    r = subprocess.run([sslib.SSBOT, "estimate", position, "--bot", bot], capture_output=True, text=True)
    e, l = EST.search(r.stdout), LEAF.search(r.stdout)
    if not e or not l: return None
    sign = 1 if e.group(3) == "White" else -1
    row = {"static": sign * float(e.group(1)), "static_end": sign * float(e.group(2)),
           "leaf": sign * float(l.group(1)), "leaf_end": sign * float(l.group(2)), "for": e.group(3)}
    g = subprocess.run([sslib.SSBOT, "goal-static", position], capture_output=True, text=True).stdout
    last = [x for x in g.splitlines() if x.startswith("attack ")]
    if last:
        for name, v in COMP.findall(last[-1]): row["c_" + name.replace("-", "_")] = float(v)
    ep = subprocess.run([sslib.SSBOT, "attack-episode", position], capture_output=True, text=True).stdout
    first = ep.split("\n", 1)[0].replace("episode", "", 1).strip() if ep else ""
    row["episode"] = first.split(",")[0].strip()   # none / general / 1 v 0 / 2 v 1 (the rest names the carrier)
    return row


STATIC_FIELDS = ["pid", "status", "for", "leaf", "leaf_end", "static", "static_end", "c_attack", "c_direct_pass",
                 "c_lead_pass", "c_keep", "c_counter", "episode"]


def cmd_static(a):
    t = sslib.Table(path("cons_static.csv"), STATIC_FIELDS, ["pid"])
    todo = [r for r in read("cons_positions.csv") if not t.has(pid=r["pid"])]
    log("%d positions" % len(todo))
    def fn(r):
        s = static_of(r["position"], a.bot)
        t.add({"pid": r["pid"], "status": "ok" if s else "error", **(s or {})})
    pool(a.jobs, fn, todo)


def subset(rows, n, seed):
    """n positions spread over the classes (the v4 ones first)."""
    rng = random.Random(seed)
    v4 = [r for r in rows if r["source"].startswith("v4 ")]
    groups = {}
    for r in rows:
        if not r["source"].startswith("v4 "): groups.setdefault((r["mover"], r["ball"]), []).append(r)
    for g in groups.values(): rng.shuffle(g)
    out = v4[:n]
    while len(out) < n and any(groups.values()):
        for k in sorted(groups):
            if groups[k] and len(out) < n: out.append(groups[k].pop())
    return out


VAL_FIELDS = ["pid", "search_depth", "width", "threads", "status", "plan", "value", "eval", "depth", "seconds",
              "wall", "expansions", "error"]


def cmd_depths(a):
    positions = read("cons_positions.csv")
    extra = (("--root-as-node",) if a.root_as_node else ()) + tuple(a.extra.split())
    name = "cons_depths%s%s.csv" % ("_node" if a.root_as_node else "", "_" + a.tag if a.tag else "")
    t = sslib.Table(path(name), VAL_FIELDS, ["pid", "search_depth", "width"])
    depths = sorted(int(x) for x in a.depths.split(","))
    deep = {r["pid"] for r in subset(positions, a.deep, 1)}
    deeper = {r["pid"] for r in subset(positions, a.deeper, 1)}
    alive = {r["pid"] for r in positions}
    for d in depths:
        rows = [r for r in positions if r["pid"] in alive and (d <= 4 or r["pid"] in (deep if d == 5 else deeper))]
        todo = [r for r in rows if not t.has(pid=r["pid"], search_depth=d, width=a.width)]
        log("depth %d: %d positions, %d to search" % (d, len(rows), len(todo)))
        def fn(r, n):
            v = sslib.search(r["position"], a.bot, depth=d, threads=n, width=a.width, timeout=a.timeout, extra=extra)
            t.add({"pid": r["pid"], "search_depth": d, "width": a.width, "threads": n, **v})
        def cost(r):   # the seconds one depth shallower: the costliest start first
            v = t.get(pid=r["pid"], search_depth=d - 1, width=a.width)
            return float(v["seconds"] or 0) if v and v["status"] == "ok" else 0.0
        sslib.schedule(todo, a.jobs, fn, cost)
        for r in rows:
            v = t.get(pid=r["pid"], search_depth=d, width=a.width)
            if not v or v["status"] != "ok": alive.discard(r["pid"])


def play(position, plan):
    """The position after `plan` (ssbot estimate --then): '' if it cannot be played."""
    return sslib.estimate_position(position, plan) if plan.strip() else position


def cmd_horizon(a):
    positions = read("cons_positions.csv")
    lines = sslib.Table(path("cons_lines.csv"), ["pid", "line_depth", "status", "steps", "line", "leaf_position",
                                                 "root_eval", "seconds"], ["pid", "line_depth"])
    checks = sslib.Table(path("cons_horizon.csv"), ["pid", "line_depth", "check_depth"] + VAL_FIELDS[3:] +
                         ["leaf", "leaf_end", "for"], ["pid", "line_depth", "check_depth"])
    chosen = subset(positions, a.count, 2)
    D = a.line_depth
    def line(r, n=1):
        if lines.has(pid=r["pid"], line_depth=D): return
        start = time.time()
        # The text of a position inside a turn loses who has moved and whether the ball was played
        # (05.11): inside a turn the line is searched from the turn's start position and the actions
        # played so far in it (--then), and only turn starts are written out as text.
        base, inturn = r["position"], []
        pos, remaining, steps, text, root_eval, status = base, D, 0, [], "", "ok"
        while remaining > 0 and steps < 4 * D + 4:
            v = sslib.search(base, a.bot, depth=remaining, threads=n, timeout=a.timeout, then=" ".join(inturn))
            if v["status"] != "ok" or not v["plan"]: status = v["status"] if v["status"] != "ok" else "noplan"; break
            if steps == 0: root_eval = v["eval"]
            if "#" in v["plan"].split(): status = "shot"; break   # the line ends in a shot: no single leaf
            nxt = play(base, " ".join(inturn + v["plan"].split()))
            if not nxt: status = "cannot play"; break
            text.append("%s: %s" % (base.split()[0], v["plan"]))
            if nxt.split()[:2] != base.split()[:2]:   # a team turn ended: a new turn start
                remaining -= 1
                base, inturn = nxt, []
            else:
                inturn += v["plan"].split()
            pos, steps = nxt, steps + 1
        if status == "ok" and remaining > 0: status = "unfinished"
        lines.add({"pid": r["pid"], "line_depth": D, "status": status,
                   "steps": steps, "line": " | ".join(text), "leaf_position": pos, "root_eval": root_eval,
                   "seconds": "%.1f" % (time.time() - start)})
        log(r["pid"], "line", status, steps, "steps")
    log("lines of depth %d from %d positions" % (D, len(chosen)))
    known = {}
    for r in read("cons_depths.csv"):
        if r["status"] == "ok" and int(r["search_depth"]) == D: known[r["pid"]] = float(r["seconds"] or 0)
    sslib.schedule(chosen, a.jobs, line, lambda r: known.get(r["pid"], 0.0))
    ks = [int(x) for x in a.check.split(",")]
    todo = [(l, k) for l in lines.rows() if l["status"] == "ok" and int(l["line_depth"]) == D for k in ks
            if not checks.has(pid=l["pid"], line_depth=D, check_depth=k)]
    log("%d checks at the leaves" % len(todo))
    def check(job, n):
        l, k = job
        v = sslib.search(l["leaf_position"], a.bot, depth=k, threads=n, timeout=a.timeout)
        s = static_of(l["leaf_position"], a.bot) or {}
        checks.add({"pid": l["pid"], "line_depth": D, "check_depth": k, **v,
                    **{x: s.get(x, "") for x in ("leaf", "leaf_end", "for")}})
    sslib.schedule(todo, a.jobs, check, lambda job: job[1])   # deeper checks first


# ---- report ----

def mean(xs): return sum(xs) / len(xs) if xs else float("nan")


def pearson(x, y):
    if len(x) < 3: return float("nan")
    mx, my = mean(x), mean(y)
    sx = math.sqrt(sum((a - mx) ** 2 for a in x)); sy = math.sqrt(sum((b - my) ** 2 for b in y))
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx and sy else float("nan")


def ranks(x):
    order = sorted(range(len(x)), key=lambda i: x[i])
    r = [0.0] * len(x)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and x[order[j + 1]] == x[order[i]]: j += 1
        for k in range(i, j + 1): r[order[k]] = (i + j) / 2
        i = j + 1
    return r


def spearman(x, y): return pearson(ranks(x), ranks(y))


def ci(xs):
    if len(xs) < 2: return float("nan")
    m = mean(xs)
    return 1.96 * math.sqrt(sum((a - m) ** 2 for a in xs) / (len(xs) - 1) / len(xs))


def cmd_report(a):
    pos = {r["pid"]: r for r in read("cons_positions.csv")}
    st = {r["pid"]: r for r in read("cons_static.csv") if r["status"] == "ok"}
    vals = {}
    for r in read("cons_depths.csv"):
        if r["status"] == "ok": vals[(r["pid"], int(r["search_depth"]))] = float(r["eval"])
    depths = sorted({d for _, d in vals})
    est = a.estimate   # the column compared: leaf (as the search values a leaf) or another
    out = []
    def P(*x): out.append(" ".join(str(y) for y in x))
    P("Static estimate (%s) against fixed-depth search; White's values; %d positions, %d with the estimate" %
      (est, len(pos), len(st)))
    classes = {"all": lambda p: True}
    for m in "wb":
        for b in ("W", "B", "free"):
            classes["%s to move, ball %s" % (m, b)] = (lambda m, b: lambda p: p["mover"] == m and p["ball"] == b)(m, b)
    for kind in sorted({r.get("episode", "") for r in st.values()}):
        classes["episode %s" % (kind or "?")] = (lambda kind: lambda p: st.get(p["pid"], {}).get("episode", "") == kind)(kind)
    P("")
    P("1. V(d) - estimate: n, bias, mean |error|, Pearson, Spearman")
    for name, f in classes.items():
        P("  " + name)
        for d in depths:
            pids = [p for p in pos if f(pos[p]) and p in st and (p, d) in vals]
            if not pids: continue
            e = [float(st[p][est]) for p in pids]; v = [vals[(p, d)] for p in pids]
            diff = [b - c for b, c in zip(v, e)]
            P("    d%d  n %4d  bias %+.3f ±%.3f  |err| %.3f  r %.2f  rho %.2f" %
              (d, len(pids), mean(diff), ci(diff), mean([abs(x) for x in diff]), pearson(e, v), spearman(e, v)))
    P("")
    P("2. V(d+1) - V(d) for the side to move (+: deeper is better for it); parity = the right to move")
    for name in ["all"] + [k for k in classes if "to move" in k]:
        f = classes[name]
        cells = []
        for d in depths[:-1]:
            xs = [(vals[(p, d + 1)] - vals[(p, d)]) * (1 if pos[p]["mover"] == "w" else -1)
                  for p in pos if f(pos[p]) and (p, d) in vals and (p, d + 1) in vals]
            if xs: cells.append("d%d->%d %+.3f (n %d)" % (d, d + 1, mean(xs), len(xs)))
        P("  %-22s %s" % (name, "  ".join(cells)))
    P("")
    P("3. At the leaf of the principal line: estimate - V(k) from the leaf, for the side at the root")
    lines = {(r["pid"], r["line_depth"]): r for r in read("cons_lines.csv")}
    hz = read("cons_horizon.csv")
    for D in sorted({r["line_depth"] for r in hz}):
        for k in sorted({int(r["check_depth"]) for r in hz if r["line_depth"] == D}):
            xs = []
            for r in hz:
                if r["line_depth"] != D or int(r["check_depth"]) != k or r["status"] != "ok" or r["leaf"] == "": continue
                side = 1 if pos[r["pid"]]["mover"] == "w" else -1
                xs.append((float(r["leaf"]) - float(r["eval"])) * side)
            if xs:
                P("  line depth %s, check depth %d: n %d, mean %+.3f ±%.3f, mean |.| %.3f, share > 0: %.0f%%" %
                  (D, k, len(xs), mean(xs), ci(xs), mean([abs(x) for x in xs]), 100 * mean([x > 0 for x in xs])))
    n_bad = sum(1 for r in lines.values() if r["status"] != "ok")
    if lines: P("  lines: %d built, %d not finished" % (len(lines) - n_bad, n_bad))
    P("")
    if depths:
        dmax = max(d for d in depths if sum(1 for p in pos if (p, d) in vals) >= 30) if \
            any(sum(1 for p in pos if (p, d) in vals) >= 30 for d in depths) else depths[-1]
        P("4. V(d%d) - estimate against the components of goal-static (side to move's view), Pearson" % dmax)
        pids = [p for p in pos if p in st and (p, dmax) in vals]
        diff = [(vals[(p, dmax)] - float(st[p][est])) * (1 if pos[p]["mover"] == "w" else -1) for p in pids]
        for c in ("c_attack", "c_direct_pass", "c_lead_pass", "c_keep", "c_counter"):
            xs = [float(st[p][c]) if st[p].get(c) not in (None, "") else float("nan") for p in pids]
            ok = [(x, y) for x, y in zip(xs, diff) if not math.isnan(x)]
            if ok: P("  %-14s r %+.2f (n %d)" % (c[2:], pearson([x for x, _ in ok], [y for _, y in ok]), len(ok)))
        eps = {}
        for p, d in zip(pids, diff): eps.setdefault(st[p].get("episode", ""), []).append(d)
        for k, xs in sorted(eps.items(), key=lambda kv: -len(kv[1])):
            P("  episode %-20s n %4d  mean %+.3f ±%.3f" % (k or "?", len(xs), mean(xs), ci(xs)))
    node = {}
    for r in read("cons_depths_node.csv"):
        if r["status"] == "ok": node[(r["pid"], int(r["search_depth"]))] = float(r["eval"])
    if node:
        P("")
        P("5. Root as searched minus root as an inner node, for the side to move (what an inner node loses)")
        for name in ["all"] + [k for k in classes if k != "all"]:
            f = classes[name]
            cells = []
            for d in sorted({d for _, d in node}):
                xs = [(vals[(p, d)] - node[(p, d)]) * (1 if pos[p]["mover"] == "w" else -1)
                      for p in pos if f(pos[p]) and (p, d) in vals and (p, d) in node]
                if xs: cells.append("d%d %+.3f ±%.3f (>0.05: %.0f%%, n %d)" % (d, mean(xs), ci(xs),
                                    100 * mean([x > 0.05 for x in xs]), len(xs)))
            if cells: P("  %-22s %s" % (name, "  ".join(cells)))
    text = "\n".join(out) + "\n"
    open(path("cons_report.txt"), "w", encoding="utf-8").write(text)
    print(text)


def main():
    _, physical = sslib.cpu_info()
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("sample", "static", "depths", "horizon", "report"):
        s = sub.add_parser(name)
        s.add_argument("--bot", default=sslib.DEFAULT_BOT)
        s.add_argument("--jobs", type=int, default=physical)
        s.add_argument("--timeout", type=float, default=6 * 3600)
    sc = sub.choices
    sc["sample"].add_argument("--games", default=os.path.join(sslib.ROOT, "games"))
    sc["sample"].add_argument("--every", type=int, default=2)
    sc["sample"].add_argument("--max", type=int, default=600)
    sc["sample"].add_argument("--seed", type=int, default=1)
    sc["depths"].add_argument("--depths", default="1,2,3,4,5,6")
    sc["depths"].add_argument("--deep", type=int, default=100, help="positions searched at depth 5")
    sc["depths"].add_argument("--deeper", type=int, default=30, help="positions searched at depth 6")
    sc["depths"].add_argument("--width", type=int, default=0)
    sc["depths"].add_argument("--extra", default="", help="more ssbot search options, e.g. '--ball-first-extra 4'")
    sc["depths"].add_argument("--tag", default="", help="results cons_depths[_node]_<tag>.csv")
    sc["depths"].add_argument("--root-as-node", action="store_true",
                              help="the root searched as an inner node: what an inner node loses (cons_depths_node.csv)")
    sc["horizon"].add_argument("--count", type=int, default=150)
    sc["horizon"].add_argument("--line-depth", type=int, default=3)
    sc["horizon"].add_argument("--check", default="1,2,3")
    sc["report"].add_argument("--estimate", default="leaf", choices=["leaf", "leaf_end", "static", "static_end"])
    a = p.parse_args()
    {"sample": cmd_sample, "static": cmd_static, "depths": cmd_depths, "horizon": cmd_horizon,
     "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    main()
