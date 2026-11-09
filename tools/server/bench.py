#!/usr/bin/env python3
"""Server benchmarks for the selective bot (docs/server_plan.md, §2).

  info     - a description of the machine (results/machine.json).
  scaling  - one search per position at a fixed depth with 1, 2, 4, ... threads:
             wall time and speed-up (the move, value and expansions must not change).
  calib    - the same single-thread searches alone and with N of them at once:
             how much the processes slow each other (curves are measured under load).
  curves   - depth against time: one search per position and budget (seconds per
             search), many processes at once.
  ladder   - fixed-depth searches d = 1..N from the position and after the bot's move
             (the opponent at the root), depth by depth; a depth that runs out of
             time stops the deeper ones of that position.

Every row is written as soon as it is computed; a rerun of the same command skips
the rows already in the CSV (rows that ended in an error are redone; --redo-timeouts
redoes those too). Positions: tests/bot_suite_cases_v4.json. Results: results/*.csv.

  python3 tools/server/bench.py info
  python3 tools/server/bench.py scaling --depth 3
  python3 tools/server/bench.py calib --depth 3 --jobs 34,40
  python3 tools/server/bench.py curves --budgets 30,60,120,240,480,960,1920,3840 --jobs 34
  python3 tools/server/bench.py ladder --depths 1,2,3,4,5,6 --moves tools/server/suite_moves.txt --jobs 34
"""
import argparse, json, math, os, sys, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sslib

RESULT = ["status", "plan", "value", "eval", "depth", "seconds", "wall", "expansions", "error"]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def out_path(a, name):
    return a.out or os.path.join(sslib.RESULTS, "%s.csv" % name)


def redo(a):
    return ("exit1", "exit2", "noparse", "exit-6", "exit-9", "exit-11") + (("timeout",) if a.redo_timeouts else ())


def cmd_info(a):
    os.makedirs(sslib.RESULTS, exist_ok=True)
    info = sslib.machine_info()
    path = os.path.join(sslib.RESULTS, "machine.json")
    json.dump(info, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    log("logical %d, physical %d ->" % (info["logical"], info["physical"]), path)


def cmd_scaling(a):
    threads = sorted({int(x) for x in a.threads.split(",")}) if a.threads else sslib.default_threads()
    tag = "scaling_d%d%s" % (a.depth, "_w%d" % a.width if a.width else "")
    t = sslib.Table(out_path(a, tag), ["case", "bot", "width", "search_depth", "threads"] + RESULT + ["speedup", "same"],
                    ["case", "bot", "width", "search_depth", "threads"], redo(a))
    log("threads", threads, "->", t.path)
    for c in sslib.cases(a.cases):
        base = None
        for n in threads:
            key = dict(case=c["id"], bot=a.bot, width=a.width, search_depth=a.depth, threads=n)
            r = t.get(**key)
            if r is None:
                r = {**key, **sslib.search(c["position"], a.bot, depth=a.depth, threads=n, width=a.width,
                                           timeout=a.timeout)}
                if base is not None and base["status"] == "ok" and r["status"] == "ok":
                    r["speedup"] = "%.2f" % (float(base["seconds"]) / max(float(r["seconds"]), 1e-3))
                    r["same"] = int((r["plan"], r["value"], r["expansions"]) ==
                                    (base["plan"], base["value"], base["expansions"]))
                elif base is None:
                    r["speedup"], r["same"] = "1.00", 1
                t.add(r)
                log(c["id"], "t%d" % n, r["status"], r["seconds"], "s x%s" % r.get("speedup", ""),
                    "" if str(r.get("same", "")) != "0" else "DIFFERENT")
            if base is None: base = r
            if r["status"] == "timeout": break   # more threads will not help a search that ran out at fewer


def run_pool(jobs, n, fn):
    with ThreadPoolExecutor(n) as ex:
        list(ex.map(fn, jobs))


def cmd_calib(a):
    loads = [int(x) for x in a.jobs.split(",")]
    t = sslib.Table(out_path(a, "calib_d%d" % a.depth), ["phase", "case", "copy"] + RESULT,
                    ["phase", "case", "copy"], redo(a))
    cs = sslib.cases(a.cases)
    def one(job):
        phase, c, copy = job
        if t.has(phase=phase, case=c["id"], copy=copy): return
        r = {"phase": phase, "case": c["id"], "copy": copy,
             **sslib.search(c["position"], a.bot, depth=a.depth, threads=1, timeout=a.timeout)}
        t.add(r)
    log("solo")
    for c in cs: one(("solo", c, 0))
    for n in loads:
        copies = max(1, math.ceil(n / len(cs)))
        jobs = [("load%d" % n, c, k) for k in range(copies) for c in cs]
        log("load %d: %d jobs" % (n, len(jobs)))
        run_pool(jobs, n, one)
    solo = {r["case"]: float(r["wall"]) for r in t.rows() if r["phase"] == "solo" and r["status"] == "ok"}
    for n in loads:
        ratios = [float(r["wall"]) / solo[r["case"]] for r in t.rows()
                  if r["phase"] == "load%d" % n and r["status"] == "ok" and r["case"] in solo]
        if ratios:
            log("load %d: slowdown mean %.2f, min %.2f, max %.2f" % (n, sum(ratios) / len(ratios), min(ratios), max(ratios)))


def cmd_curves(a):
    budgets = [float(x) for x in a.budgets.split(",")]
    t = sslib.Table(out_path(a, "curves"), ["case", "bot", "budget", "threads"] + RESULT,
                    ["case", "bot", "budget", "threads"], redo(a))
    jobs = [(c, b) for b in sorted(budgets, reverse=True) for c in sslib.cases(a.cases)]   # longest first
    def fn(job):
        c, b = job
        key = dict(case=c["id"], bot=a.bot, budget=b, threads=a.threads)
        if t.has(**key): return
        r = {**key, **sslib.search(c["position"], a.bot, seconds=b, threads=a.threads, timeout=b * 3 + 600)}
        t.add(r)
        log(c["id"], b, r["status"], "depth", r["depth"], r["eval"], r["plan"])
    log("%d jobs, %d at once -> %s" % (len(jobs), a.jobs, t.path))
    run_pool(jobs, a.jobs, fn)


def cmd_ladder(a):
    depths = sorted(int(x) for x in a.depths.split(","))
    moves = sslib.read_moves(a.moves)
    extra = ("--root-as-node",) if a.root_as_node else ()
    t = sslib.Table(out_path(a, "ladder%s%s" % ("_w%d" % a.width if a.width else "", "_node" if a.root_as_node else "")),
                    ["case", "bot", "width", "after", "search_depth", "threads"] + RESULT,
                    ["case", "bot", "width", "after", "search_depth"], redo(a))
    lines = []   # (case, after): the position, and the bot's move searched after (the opponent at the root)
    for c in sslib.cases(a.cases):
        lines.append((c, ""))
        if c["id"] in moves: lines.append((c, moves[c["id"]]))
    alive = set(range(len(lines)))
    for d in depths:
        jobs = [i for i in sorted(alive)]
        if not jobs: break
        log("depth %d: %d searches on %d cores (costliest first; the last get the free cores as threads)" %
            (d, len(jobs), a.jobs))
        def fn(i, n):
            c, then = lines[i]
            key = dict(case=c["id"], bot=a.bot, width=a.width, after=then, search_depth=d)
            r = t.get(**key)
            if r is None:
                n = a.threads or n
                r = {**key, "threads": n,
                     **sslib.search(c["position"], a.bot, depth=d, threads=n, then=then, width=a.width,
                                    timeout=a.timeout, extra=extra)}
                t.add(r)
                log(c["id"], "after" if then else "root", "d%d" % d, r["status"], r["seconds"], "s", "t%d" % n,
                    r["eval"], r["plan"])
            if r["status"] != "ok": alive.discard(i)
        def cost(i):   # seconds one depth shallower
            c, then = lines[i]
            v = t.get(case=c["id"], bot=a.bot, width=a.width, after=then, search_depth=d - 1)
            return float(v["seconds"] or 0) if v and v.get("status") == "ok" else 0.0
        sslib.schedule(jobs, a.jobs, fn, cost)


def main():
    _, physical = sslib.cpu_info()
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    for name in ("scaling", "calib", "curves", "ladder"):
        s = sub.add_parser(name)
        s.add_argument("--bot", default=sslib.DEFAULT_BOT)
        s.add_argument("--cases", default="", help="comma-separated case ids (default: all)")
        s.add_argument("--out", default="", help="CSV (default: results/<command>.csv)")
        s.add_argument("--redo-timeouts", action="store_true")
    sc = sub.choices
    sc["scaling"].add_argument("--depth", type=int, default=3)
    sc["scaling"].add_argument("--width", type=int, default=0)
    sc["scaling"].add_argument("--threads", default="", help="list (default: 1,2,4,... up to the physical and logical cores)")
    sc["scaling"].add_argument("--timeout", type=float, default=4 * 3600)
    sc["calib"].add_argument("--depth", type=int, default=3)
    sc["calib"].add_argument("--jobs", default=str(physical), help="loads, comma-separated (e.g. 34,40)")
    sc["calib"].add_argument("--timeout", type=float, default=3600)
    sc["curves"].add_argument("--budgets", default="30,60,120,240,480,960,1920,3840")
    sc["curves"].add_argument("--jobs", type=int, default=physical)
    sc["curves"].add_argument("--threads", type=int, default=1)
    sc["ladder"].add_argument("--depths", default="1,2,3,4,5")
    sc["ladder"].add_argument("--width", type=int, default=0)
    sc["ladder"].add_argument("--root-as-node", action="store_true",
                              help="the root as an inner node (ssbot --root-as-node); results ladder_node.csv")
    sc["ladder"].add_argument("--jobs", type=int, default=physical)
    sc["ladder"].add_argument("--cores", type=int, default=physical)
    sc["ladder"].add_argument("--threads", type=int, default=0, help="per search (default: 1, the free cores to the last ones)")
    sc["ladder"].add_argument("--timeout", type=float, default=6 * 3600)
    sc["ladder"].add_argument("--moves", default=os.path.join(sslib.ROOT, "tools", "server", "suite_moves.txt"),
                              help="file of 'case_id|actions' lines")
    a = p.parse_args()
    if hasattr(a, "cases"): a.cases = [x for x in a.cases.split(",") if x]
    {"info": cmd_info, "scaling": cmd_scaling, "calib": cmd_calib, "curves": cmd_curves, "ladder": cmd_ladder}[a.cmd](a)


if __name__ == "__main__":
    main()
