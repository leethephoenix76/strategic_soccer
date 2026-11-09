#!/usr/bin/env python3
"""Reference values of candidate moves for the test positions (docs/server_plan.md, §3).

  candidates - gather the candidates of every position: the moves of tools/server/suite_moves.txt,
               the user's moves (--user-moves), the plans of the benchmark CSVs (curves, ladder),
               and every root decision a wide search opens, completed by the bot. A candidate is
               cut after its first random action (pass, shot, tackle): what follows a random
               action depends on its outcome, so the verifying search chooses it. Candidates
               ending in the same states are merged.
  verify     - every candidate at levels "width x depth" (the bot's search from each outcome
               of the candidate with --width W --turn-depth D, no time limit, one thread per
               search, many at once). Outcomes less likely than --min-p are searched at the first
               level only (their value is carried to the deeper levels). Value of a candidate for the side to move =
               sum over outcomes of p * value (a goal = 1).
  report     - per position: candidates x levels, the rank at each level, whether the order of
               the best ones has settled, and the error of each bot move (best - its value).

Rows are written as computed; a rerun continues. Results: results/ref_*.csv, results/ref_report.txt.

  python3 tools/server/reference.py candidates --root-width 16 --root-depth 3
  python3 tools/server/reference.py verify --levels 8x3,8x4,16x4,8x5,32x4,16x5,8x6 --jobs 34
  python3 tools/server/reference.py report
"""
import argparse, csv, glob, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sslib

GOAL = 7          # OutcomeKind::Goal (src/engine/outcomes.h)
OUTCOME = re.compile(r"^outcome of (\S+): kind (\d+) p ([\d.]+) -> (.*)$")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def path(name):
    return os.path.join(sslib.RESULTS, name)


def expand(position, actions):
    """Plays `actions` (ssbot --then --show-outcomes). Returns (the actions up to and including
    the first action with several outcomes, [(kind, p, position)] of its outcomes), or the whole
    list and its final state with p 1 when no action is random; None if it cannot be played."""
    r = subprocess.run([sslib.SSBOT, "estimate", position, "--bot", sslib.DEFAULT_BOT, "--then", " ".join(actions),
                        "--show-outcomes"], capture_output=True, text=True)
    if r.returncode != 0: return None
    lines = [OUTCOME.match(l) for l in r.stderr.splitlines()]
    lines = [m.groups() for m in lines if m]
    j = 0
    last = None
    for i, a in enumerate(actions):
        group = []
        while j < len(lines) and lines[j][0] == a:
            group.append((int(lines[j][1]), float(lines[j][2]), lines[j][3]))
            j += 1
        if not group: return None
        if len(group) > 1: return actions[:i + 1], group
        last = group
    return actions, [(last[0][0], 1.0, last[0][2])]


def cmd_candidates(a):
    cands = sslib.Table(path("ref_candidates.csv"), ["case", "cand", "sources", "actions", "full", "outcomes", "key"],
                        ["case", "cand"])
    outs = sslib.Table(path("ref_outcomes.csv"), ["case", "cand", "k", "kind", "p", "position"], ["case", "cand", "k"])
    roots = sslib.Table(path("ref_root_search.csv"), ["case", "action", "lower", "upper", "status", "plan", "error"],
                        ["case", "action"])
    suite = sslib.read_moves(a.suite_moves)
    user = sslib.read_moves(a.user_moves)
    plans = {}   # case -> [(source, plan)]
    for pattern in a.plans.split(","):
        for f in sorted(glob.glob(pattern)):
            for r in csv.DictReader(open(f, newline="", encoding="utf-8")):
                if r.get("status", "ok") != "ok" or not r.get("plan") or r.get("after"): continue
                tag = "budget %s" % r["budget"] if r.get("budget") else "d%s" % r.get("search_depth", "?")
                plans.setdefault(r["case"], []).append(("%s %s" % (os.path.basename(f).split(".")[0], tag), r["plan"]))
    for c in sslib.cases(a.cases):
        pos, cid = c["position"], c["id"]
        found = []   # (source, plan)
        if cid in suite: found.append(("suite", suite[cid]))
        if cid in user: found.append(("user", user[cid]))
        found += plans.get(cid, [])
        # root decisions of a wide search, each completed by the bot
        if a.root_depth > 0:
            if not any(r["case"] == cid for r in roots.rows()):
                log(cid, "root search w%d d%d" % (a.root_width, a.root_depth))
                r = sslib.search(pos, sslib.DEFAULT_BOT, depth=a.root_depth, threads=a.threads, width=a.root_width,
                                 root_moves=True, timeout=a.timeout)
                rows = [{"case": cid, "action": act, "lower": lo, "upper": hi, "status": "open"} for lo, hi, act in r["root_moves"]]
                if not rows: rows = [{"case": cid, "action": "", "status": r["status"], "error": r["error"]}]
                for row in rows: roots.add(row)
            todo = [r for r in roots.rows() if r["case"] == cid and r["action"] and r["status"] == "open"]
            head = pos.split()[:2]   # side to move and turn number
            def complete(row):
                e = expand(pos, [row["action"]])
                if e is None or len(e[1]) > 1 or e[1][0][2].split()[:2] != head:
                    roots.add(dict(row, status="done", plan=""))   # random or ends the turn: nothing to complete
                    return
                rest = sslib.search(pos, sslib.DEFAULT_BOT, depth=a.root_depth, threads=1, width=a.root_width,
                                    then=row["action"], timeout=a.timeout)
                row = dict(row, status="done" if rest["status"] == "ok" else rest["status"], plan=rest["plan"],
                           error=rest["error"])
                roots.add(row)
            with ThreadPoolExecutor(a.jobs) as ex: list(ex.map(complete, todo))
            for r in roots.rows():
                if r["case"] == cid and r["action"] and r["status"] in ("done", "timeout"):
                    found.append(("root", (r["action"] + " " + r.get("plan", "")).strip()))
        # cut, expand, merge
        known = {r["key"]: r for r in cands.rows() if r["case"] == cid}
        n = len(known)
        for source, plan in found:
            actions = plan.split()
            e = expand(pos, actions)
            if e is None:
                log(cid, "cannot play", source, plan)
                continue
            cut, leaves = e
            key = "|".join(sorted(p for _, _, p in leaves)) + "|" + " ".join(cut[-1:])
            if key in known:
                row = known[key]
                if source not in row["sources"].split("; "):
                    row["sources"] += "; " + source
                    cands._rewrite(list(cands.done.values()))
                continue
            row = {"case": cid, "cand": "c%02d" % n, "sources": source, "actions": " ".join(cut), "full": plan,
                   "outcomes": len(leaves), "key": key}
            n += 1
            cands.add(row)
            known[key] = row
            for k, (kind, p, position) in enumerate(leaves):
                outs.add({"case": cid, "cand": row["cand"], "k": k, "kind": kind, "p": p, "position": position})
        log(cid, "%d candidates" % n)


def levels(text):
    return [tuple(int(x) for x in l.split("x")) for l in text.split(",") if l]


def cmd_verify(a):
    vals = sslib.Table(path("ref_values.csv"), ["case", "cand", "k", "width", "search_depth", "status", "plan", "value",
                                               "eval", "depth", "seconds", "wall", "expansions", "error"],
                       ["case", "cand", "k", "width", "search_depth"],
                       ("exit1", "exit2", "noparse") + (("timeout",) if a.redo_timeouts else ()))
    cases = {c["id"]: c for c in sslib.cases(a.cases)}
    outs = [r for r in csv.DictReader(open(path("ref_outcomes.csv"), newline="", encoding="utf-8")) if r["case"] in cases]
    actions = {(r["case"], r["cand"]): r["actions"]
               for r in csv.DictReader(open(path("ref_candidates.csv"), newline="", encoding="utf-8"))}

    for w, d in levels(a.levels):
        def dead(o):   # a cheaper level (narrower and shallower) of this outcome ran out of time
            return any(r["status"] == "timeout" and int(r["width"]) <= w and int(r["search_depth"]) <= d
                       for r in vals.rows() if (r["case"], r["cand"], r["k"]) == (o["case"], o["cand"], o["k"]))
        first = (w, d) == levels(a.levels)[0]
        jobs = [o for o in outs if int(o["kind"]) != GOAL and (first or float(o["p"]) >= a.min_p)
                and not vals.has(case=o["case"], cand=o["cand"], k=o["k"], width=w, search_depth=d)]
        skip = [o for o in jobs if dead(o)]
        for o in skip:
            vals.add({"case": o["case"], "cand": o["cand"], "k": o["k"], "width": w, "search_depth": d, "status": "skipped"})
        jobs = [o for o in jobs if o not in skip]
        jobs.sort(key=lambda o: -float(o["p"]))   # likely outcomes first
        log("level %dx%d: %d searches (%d skipped)" % (w, d, len(jobs), len(skip)))
        def fn(o, n):
            # From the position and the candidate's actions with this outcome of the last one (ssbot
            # --outcome): the engine state itself, not its text (a moving or deflected ball can lie
            # under a player, which the position parser refuses; 05.11).
            r = sslib.search(cases[o["case"]]["position"], a.bot, depth=d, width=w, threads=n, timeout=a.timeout,
                             then=actions[(o["case"], o["cand"])], extra=("--outcome", o["k"]))
            vals.add({"case": o["case"], "cand": o["cand"], "k": o["k"], "width": w, "search_depth": d, **r})
            if r["status"] != "ok": log(o["case"], o["cand"], o["k"], "%dx%d" % (w, d), r["status"])
        def cost(o):   # the seconds of this outcome's last finished level (cheaper), else its probability
            prev = [r for r in vals.rows() if (r["case"], r["cand"], r["k"]) == (o["case"], o["cand"], o["k"])
                    and r.get("status") == "ok" and r.get("seconds")]
            return max((float(r["seconds"]) for r in prev), default=float(o["p"]) * 1e-3)
        sslib.schedule(jobs, a.jobs, fn, cost)
        cmd_report(a, quiet=True)


def cmd_drop_midturn(a):
    """Once (05.11): drop the values of candidates cut inside a turn - before 05.11 they were searched
    from the text of the state, which loses who has moved and whether the ball was played; verify
    then searches them again from the position and the actions (--then, --outcome)."""
    vals = sslib.Table(path("ref_values.csv"), ["case", "cand", "k", "width", "search_depth", "status", "plan", "value",
                                               "eval", "depth", "seconds", "wall", "expansions", "error"],
                       ["case", "cand", "k", "width", "search_depth"])
    actions = {(r["case"], r["cand"]): r["actions"]
               for r in csv.DictReader(open(path("ref_candidates.csv"), newline="", encoding="utf-8"))}
    mid = {k for k, v in actions.items() if v.split()[-1] not in ("end", "#")}
    keep = [r for r in vals.rows() if (r["case"], r["cand"]) not in mid]
    log("candidates cut inside a turn: %d of %d; values dropped %d, kept %d" %
        (len(mid), len(actions), len(vals.rows()) - len(keep), len(keep)))
    vals._rewrite(keep)


def cmd_report(a, quiet=False):
    cases = {c["id"]: c for c in sslib.cases(a.cases)}
    cands = [r for r in csv.DictReader(open(path("ref_candidates.csv"), newline="", encoding="utf-8")) if r["case"] in cases]
    outs = list(csv.DictReader(open(path("ref_outcomes.csv"), newline="", encoding="utf-8")))
    vals = list(csv.DictReader(open(path("ref_values.csv"), newline="", encoding="utf-8"))) \
        if os.path.exists(path("ref_values.csv")) else []
    lv = levels(a.levels)
    val = {(r["case"], r["cand"], r["k"], int(r["width"]), int(r["search_depth"])): r for r in vals}
    summary, text = [], []
    for cid, c in cases.items():
        side = sslib.mover(c["position"])
        mine = [r for r in cands if r["case"] == cid]
        table = {}   # cand -> {level: score}
        for r in mine:
            os_ = [o for o in outs if (o["case"], o["cand"]) == (cid, r["cand"])]
            for w, d in lv:
                total, ok = 0.0, True
                for o in os_:
                    p = float(o["p"])
                    if int(o["kind"]) == GOAL: total += p; continue
                    v = val.get((cid, r["cand"], o["k"], w, d))
                    if (not v or v["status"] != "ok") and p < a.min_p:   # a rare outcome: its deepest earlier value
                        for l in reversed(lv[:lv.index((w, d))]):
                            u = val.get((cid, r["cand"], o["k"]) + l)
                            if u and u["status"] == "ok": v = u; break
                    if not v or v["status"] != "ok": ok = False; break
                    e = float(v["eval"])   # + better for White
                    total += p * (e if side == "w" else -e)
                if ok and os_: table.setdefault(r["cand"], {})[(w, d)] = total
        done = [l for l in lv if any(l in t for t in table.values())]
        text.append("== %s (%s to move): %d candidates; levels %s" % (cid, "White" if side == "w" else "Black", len(mine),
                    " ".join("%dx%d" % l for l in done)))
        ranks = {}
        for l in done:
            order = sorted((cd for cd in table if l in table[cd]), key=lambda cd: -table[cd][l])
            for i, cd in enumerate(order): ranks.setdefault(cd, {})[l] = i + 1
        last = done[-1] if done else None
        prev = done[-2] if len(done) > 1 else None
        if last:
            top = lambda l: [cd for cd in sorted(table, key=lambda cd: ranks.get(cd, {}).get(l, 999))][:a.top]
            settled = prev is not None and top(prev) == top(last)
            best = max((table[cd][last] for cd in table if last in table[cd]), default=None)
            text.append("   top %d %s between the last two levels" % (a.top, "SETTLED" if settled else "still changing"))
        for r in sorted(mine, key=lambda r: ranks.get(r["cand"], {}).get(last, 999)):
            sc = table.get(r["cand"], {})
            cells = " ".join(("%+.3f(%d)" % (sc[l], ranks[r["cand"]][l])) if l in sc else "   -     " for l in done)
            err = (best - sc[last]) if last and last in sc and best is not None else None
            text.append("   %s %s  [%s]  %s%s" % (r["cand"], cells, r["sources"], r["actions"],
                                                 "" if err is None else "  err %.3f" % err))
            summary.append({"case": cid, "cand": r["cand"], "sources": r["sources"], "actions": r["actions"],
                            "outcomes": r["outcomes"],
                            **{"%dx%d" % l: ("%.4f" % sc[l]) if l in sc else "" for l in lv},
                            "rank_last": ranks.get(r["cand"], {}).get(last, ""),
                            "error_last": "" if err is None else "%.4f" % err})
    open(path("ref_report.txt"), "w", encoding="utf-8").write("\n".join(text) + "\n")
    if summary:
        with open(path("ref_summary.csv"), "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(summary[0].keys()))
            w.writeheader(); w.writerows(summary)
    if not quiet: print("\n".join(text))


def main():
    _, physical = sslib.cpu_info()
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("candidates", "verify", "report", "drop-midturn"):
        s = sub.add_parser(name)
        s.add_argument("--cases", default="")
        s.add_argument("--levels", default="8x3,8x4,16x3,8x5,16x4", help="width x depth, cheapest first")
        s.add_argument("--min-p", type=float, default=0.02,
                       help="outcomes less likely than this are searched at the first level only")
    s = sub.choices["candidates"]
    s.add_argument("--suite-moves", default=os.path.join(sslib.ROOT, "tools", "server", "suite_moves.txt"))
    s.add_argument("--user-moves", default=os.path.join(sslib.ROOT, "tools", "server", "user_moves.txt"))
    s.add_argument("--plans", default=",".join(os.path.join(sslib.RESULTS, x) for x in ("curves*.csv", "ladder*.csv")))
    s.add_argument("--root-width", type=int, default=16)
    s.add_argument("--root-depth", type=int, default=3, help="0: no root search")
    s.add_argument("--threads", type=int, default=physical, help="of the root search")
    s.add_argument("--jobs", type=int, default=physical)
    s.add_argument("--timeout", type=float, default=4 * 3600)
    s = sub.choices["verify"]
    s.add_argument("--bot", default=sslib.DEFAULT_BOT)
    s.add_argument("--jobs", type=int, default=physical)
    s.add_argument("--timeout", type=float, default=6 * 3600)
    s.add_argument("--redo-timeouts", action="store_true")
    sub.choices["report"].add_argument("--top", type=int, default=3)
    sub.choices["verify"].add_argument("--top", type=int, default=3)
    a = p.parse_args()
    a.cases = [x for x in a.cases.split(",") if x]
    {"candidates": cmd_candidates, "verify": cmd_verify, "report": cmd_report, "drop-midturn": cmd_drop_midturn}[a.cmd](a)


if __name__ == "__main__":
    main()
