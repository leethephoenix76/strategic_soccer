#!/usr/bin/env python3
"""Round-robin bot tournament on the server (08.11.2026, the user's decision).

Every pair of bots plays both colours on every seed: episodes of 22 team half-turns
from the 0-0 kickoff, one thread per bot, many episodes at once (one per physical core).
The games are ordered by seed: after the first seed the round robin is already complete,
each further seed doubles it. Resumable: finished episodes are skipped.

  python3 tools/server/tournament.py run     [--seeds 7,19,31,43] [--time 120] [--jobs N] [--turns 22]
  python3 tools/server/tournament.py status   progress and the standings so far (also while running)
  python3 tools/server/tournament.py report   tables into tournament/report/ (run also writes them)
  python3 tools/server/tournament.py list     the bots and the episodes, nothing is played
  python3 tools/server/tournament.py smoke    a 2-minute check of the whole chain (tournament_smoke/)

Folder (default tournament/, or $SS_TOURNAMENT):
  ssbot               the copy of the binary every episode uses
  games/<id>.ssn      records (open in soccer.exe), <id>.csv - search trace, <id>.log - output
  report/standings.txt  the table: points, goals, xG, shots, possession, per bot
  report/pairs.txt      bot x bot: points and the xG difference per episode
  report/games.csv      one row per episode
  report/stats_all.txt  `ssbot stats` over all records
"""
import argparse, csv, itertools, math, os, shutil, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILT = os.path.join(ROOT, "build", "linux", "ssbot")

P = "goal-sel-run-geo-cover-timed-rbr-ext2w-"
BOTS = [  # short name, preset without the -b8-tNNN suffix
    ("pkr",   P + "pkr"),          # bot by default since 02.11.2026
    ("pv10",  P + "pkr-pv10"),     # possession value W = 0.1
    ("pv20",  P + "pkr-pv"),       # W = 0.2
    ("pv30",  P + "pkr-pv30"),     # W = 0.3
    ("bf",    P + "pkr-bf"),       # ball first in inner nodes
    ("mo",    P + "pkr-mo"),       # main outcome only
    ("ext2w", P[:-1]),             # bot by default 01.11 - 02.11.2026
    ("fb",    "goal-sel-run-fb"),  # bot by default until 29.10.2026 (lower mark)
]
STATS_ROWS = {"goals": "goals", "shots": "shots", "expected goals (xG)": "xg", "passes": "passes",
              "turns with the ball": "ball_turns", "balls lost in play": "lost",
              "tackles suffered": "tackled", "ball progress, %": "progress"}


def folder(args):
    return os.path.abspath(args.out or os.environ.get("SS_TOURNAMENT", os.path.join(ROOT, "tournament")))


def preset(name, seconds):
    return dict(BOTS)[name] + "-b8-t" + str(seconds)


def episodes(seeds):
    """(id, white, black, seed), seed by seed: each seed is a complete double round robin."""
    out = []
    for seed in seeds:
        for a, b in itertools.combinations([n for n, _ in BOTS], 2):
            out.append(("%s_vs_%s_s%d" % (a, b, seed), a, b, seed))
            out.append(("%s_vs_%s_s%d" % (b, a, seed), b, a, seed))
    return out


def physical_cores():
    try:
        cpus = os.sched_getaffinity(0)
    except AttributeError:
        return os.cpu_count() or 1
    cores = set()
    for c in cpus:
        try:
            base = "/sys/devices/system/cpu/cpu%d/topology/" % c
            cores.add((open(base + "physical_package_id").read().strip(), open(base + "core_id").read().strip()))
        except OSError:
            cores.add(("?", c))
    return max(1, len(cores))


def finished(ssbot, games, gid):
    ssn, trace = os.path.join(games, gid + ".ssn"), os.path.join(games, gid + ".csv")
    if not (os.path.exists(ssn) and os.path.exists(trace)):
        return False
    return subprocess.run([ssbot, "replay", ssn], capture_output=True).returncode == 0


def play(ssbot, games, ep, seconds, turns, override, lock, progress):
    gid, white, black, seed = ep
    tmp = os.path.join(games, gid + ".part")
    args = [ssbot, "selfplay", "--white", preset(white, seconds), "--black", preset(black, seconds),
            "--episode-turns", str(turns), "--seed", str(seed), "--threads", "1",
            "--out", tmp + ".ssn", "--trace", tmp + ".csv", "--silent"] + override
    start = time.time()
    limit = turns * max(seconds, 1) * 2 + 900
    with open(os.path.join(games, gid + ".log"), "w") as log:
        log.write(" ".join(args) + "\n")
        log.flush()
        try:
            rc = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=limit).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    ok = rc == 0 and os.path.exists(tmp + ".ssn")
    if ok:
        os.replace(tmp + ".csv", os.path.join(games, gid + ".csv"))
        os.replace(tmp + ".ssn", os.path.join(games, gid + ".ssn"))
    score = ""
    if ok:
        s = game_stats(ssbot, os.path.join(games, gid + ".ssn"))
        if s["W"]:
            score = "%d:%d  xG %.2f:%.2f" % (s["W"]["goals"], s["B"]["goals"], s["W"]["xg"], s["B"]["xg"])
    with lock:
        progress["done" if ok else "failed"] += 1
        print("%s %-22s %-20s %5.1f min  (done %d, failed %d, of %d)%s" % (
            time.strftime("%H:%M"), gid, score, (time.time() - start) / 60, progress["done"], progress["failed"],
            progress["total"], "" if ok else "  FAILED rc=%s, see games/%s.log" % (rc, gid)), flush=True)


def game_stats(ssbot, ssn):
    """{'W': {...}, 'B': {...}} from `ssbot stats` of one record."""
    out = subprocess.run([ssbot, "stats", ssn], capture_output=True, text=True).stdout
    side = {"W": {}, "B": {}}
    for line in out.splitlines():
        for label, key in STATS_ROWS.items():
            if line.startswith(label + " ") or line.startswith(label + "\t"):
                parts = line[len(label):].split()
                if len(parts) >= 2:
                    side["W"][key], side["B"][key] = float(parts[0]), float(parts[1])
    return side


def collect(ssbot, games, seeds):
    rows = []
    for gid, white, black, seed in episodes(seeds):
        ssn = os.path.join(games, gid + ".ssn")
        if not (os.path.exists(ssn) and os.path.exists(os.path.join(games, gid + ".csv"))):
            continue
        s = game_stats(ssbot, ssn)
        if not s["W"]:
            continue
        rows.append({"id": gid, "white": white, "black": black, "seed": seed,
                     **{"w_" + k: v for k, v in s["W"].items()}, **{"b_" + k: v for k, v in s["B"].items()}})
    return rows


def mean_se(values):
    n = len(values)
    if n == 0:
        return 0.0, 0.0
    m = sum(values) / n
    if n < 2:
        return m, 0.0
    var = sum((v - m) ** 2 for v in values) / (n - 1)
    return m, math.sqrt(var / n)


def report(ssbot, base, seeds, quiet=False):
    games = os.path.join(base, "games")
    rows = collect(ssbot, games, seeds)
    rep = os.path.join(base, "report")
    os.makedirs(rep, exist_ok=True)
    names = [n for n, _ in BOTS]
    if rows:
        keys = list(rows[0].keys())
        with open(os.path.join(rep, "games.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)

    per = {n: {"n": 0, "w": 0, "d": 0, "l": 0, "gf": 0.0, "ga": 0.0, "xgf": 0.0, "xga": 0.0, "xgd": [],
               "shots": 0.0, "shots_a": 0.0, "passes": 0.0, "ball_turns": 0.0, "lost": 0.0, "tackled": 0.0}
           for n in names}
    pair = {(a, b): {"n": 0, "pts": 0, "gf": 0.0, "ga": 0.0, "xgd": []} for a in names for b in names if a != b}
    for r in rows:
        for me, op, m, o in (("white", "black", "w_", "b_"), ("black", "white", "b_", "w_")):
            a, b = r[me], r[op]
            gf, ga = r[m + "goals"], r[o + "goals"]
            s = per[a]
            s["n"] += 1
            s["w" if gf > ga else "l" if gf < ga else "d"] += 1
            s["gf"] += gf; s["ga"] += ga
            s["xgf"] += r[m + "xg"]; s["xga"] += r[o + "xg"]; s["xgd"].append(r[m + "xg"] - r[o + "xg"])
            s["shots"] += r[m + "shots"]; s["shots_a"] += r[o + "shots"]
            for k in ("passes", "ball_turns", "lost", "tackled"):
                s[k] += r.get(m + k, 0.0)
            p = pair[(a, b)]
            p["n"] += 1
            p["pts"] += 3 if gf > ga else 1 if gf == ga else 0
            p["gf"] += gf; p["ga"] += ga; p["xgd"].append(r[m + "xg"] - r[o + "xg"])

    def pts(s):
        return 3 * s["w"] + s["d"]
    order = sorted(names, key=lambda n: (-pts(per[n]) / max(per[n]["n"], 1), -mean_se(per[n]["xgd"])[0]))
    total = len(episodes(seeds))
    lines = ["Tournament %s: %d of %d episodes finished, seeds %s" % (
        os.path.basename(base), len(rows), total, ",".join(map(str, seeds))), ""]
    lines.append("%-6s %4s %8s %5s %6s %7s %7s %7s %14s %6s %6s %6s %6s %6s" % (
        "bot", "ep", "W-D-L", "pts", "pts/ep", "goals", "xG", "xG agst", "xG diff/ep", "shots", "sh agst",
        "passes", "ball", "lost"))
    for n in order:
        s = per[n]
        k = max(s["n"], 1)
        m, se = mean_se(s["xgd"])
        lines.append("%-6s %4d %8s %5d %6.2f %7s %7.2f %7.2f %+7.3f ±%.3f %6.2f %6.2f %6.2f %6.2f %6.2f" % (
            n, s["n"], "%d-%d-%d" % (s["w"], s["d"], s["l"]), pts(s), pts(s) / k,
            "%d:%d" % (s["gf"], s["ga"]), s["xgf"] / k, s["xga"] / k, m, se, s["shots"] / k, s["shots_a"] / k,
            s["passes"] / k, s["ball_turns"] / k, s["lost"] / k))
    lines += ["", "goals, xG, shots, passes - per episode except goals (total); ball - team turns with the ball;",
              "lost - balls lost in play; xG diff/ep - mean (own xG - opponent's xG) ± its standard error.", ""]
    lines.append("Bots:")
    for n, full in BOTS:
        lines.append("  %-6s %s-b8-t..." % (n, full))
    text = "\n".join(lines) + "\n"
    open(os.path.join(rep, "standings.txt"), "w").write(text)

    w = 9
    pl = ["Pairs: row against column. Points per episode (3/1/0), then the mean xG difference per episode.", ""]
    pl.append("points/ep".ljust(8) + "".join(c.rjust(w) for c in order))
    for a in order:
        pl.append(a.ljust(8) + "".join(
            ("-" if a == b else ("%.2f" % (pair[(a, b)]["pts"] / pair[(a, b)]["n"]) if pair[(a, b)]["n"] else "."))
            .rjust(w) for b in order))
    pl += ["", "xG diff/ep".ljust(8) + "".join(c.rjust(w) for c in order)]
    for a in order:
        pl.append(a.ljust(8) + "".join(
            ("-" if a == b else ("%+.2f" % mean_se(pair[(a, b)]["xgd"])[0] if pair[(a, b)]["n"] else "."))
            .rjust(w) for b in order))
    pl += ["", "goals".ljust(8) + "".join(c.rjust(w) for c in order)]
    for a in order:
        pl.append(a.ljust(8) + "".join(
            ("-" if a == b else ("%d:%d" % (pair[(a, b)]["gf"], pair[(a, b)]["ga"]) if pair[(a, b)]["n"] else "."))
            .rjust(w) for b in order))
    open(os.path.join(rep, "pairs.txt"), "w").write("\n".join(pl) + "\n")

    files = [os.path.join(games, r["id"] + ".ssn") for r in rows]
    if files:
        out = subprocess.run([ssbot, "stats"] + files, capture_output=True, text=True).stdout
        open(os.path.join(rep, "stats_all.txt"), "w").write(out)
    if not quiet:
        print(text)
        print("\n".join(pl))
        print("\nReport: %s" % rep)
    return len(rows)


def binary(base, smoke=False):
    """The tournament's own copy of ssbot: a later build does not mix versions."""
    os.makedirs(base, exist_ok=True)
    mine = os.path.join(base, "ssbot")
    if not os.path.exists(mine) or smoke:
        if not os.path.exists(BUILT):
            sys.exit("missing %s: run ./build_linux.sh first" % BUILT)
        shutil.copy2(BUILT, mine)
    listed = subprocess.run([mine, "bots"], capture_output=True, text=True).stdout
    open(os.path.join(base, "bots.txt"), "w").write(listed)
    have = {line.split()[0] for line in listed.splitlines() if line.strip()}
    missing = [full for _, full in BOTS if full + "-b8-t120" not in have]
    if missing:
        sys.exit("not in %s: %s\nrebuild, delete that copy and start again" % (mine, ", ".join(missing)))
    return mine


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["run", "status", "report", "list", "smoke"])
    ap.add_argument("--seeds", default="", help="default 7,19,31,43; status/report: as in the run")
    ap.add_argument("--time", type=int, default=120, choices=[90, 120, 180, 300],
                    help="seconds per team half-turn (a preset of the bots)")
    ap.add_argument("--turns", type=int, default=22, help="team half-turns per episode")
    ap.add_argument("--jobs", type=int, default=0, help="episodes at once (default: physical cores - 1)")
    ap.add_argument("--out", default="", help="tournament folder (default tournament/)")
    args = ap.parse_args()
    seeds_text = args.seeds
    settings = os.path.join(folder(args), "settings.txt")
    if not seeds_text and args.what in ("status", "report") and os.path.exists(settings):
        for line in open(settings):
            if line.startswith("seeds "):
                seeds_text = line.split()[1]
    seeds = [int(s) for s in (seeds_text or "7,19,31,43").split(",") if s]
    override = []
    if args.what == "smoke":
        args.out = args.out or os.path.join(ROOT, "tournament_smoke")
        seeds, args.turns, override = [7], 2, ["--wtime", "0.3", "--btime", "0.3"]
        if os.path.isdir(args.out):
            shutil.rmtree(args.out)
    base = folder(args)
    eps = episodes(seeds)
    if args.what == "list":
        for n, full in BOTS:
            print("%-6s %s-b8-t%d" % (n, full, args.time))
        print("%d episodes:" % len(eps))
        for e in eps:
            print("  %s" % e[0])
        return
    ssbot = binary(base, smoke=args.what == "smoke")
    games = os.path.join(base, "games")
    os.makedirs(games, exist_ok=True)
    if args.what in ("status", "report"):
        done = report(ssbot, base, seeds, quiet=False)
        logs = {f[:-4] for f in os.listdir(games) if f.endswith(".log")}
        print("\nfinished %d of %d; started and not finished: %d" % (done, len(eps), len(logs) - done))
        return

    jobs = args.jobs or max(1, physical_cores() - 1)
    todo = [e for e in eps if not finished(ssbot, games, e[0])]
    est = math.ceil(len(todo) / jobs) * args.turns * args.time / 60
    print("%s: %d bots, %d episodes (%d to play), %d at once, %d s per half-turn, %d half-turns;"
          " at most ~%.1f h" % (base, len(BOTS), len(eps), len(todo), jobs, args.time, args.turns, est / 60),
          flush=True)
    open(os.path.join(base, "settings.txt"), "w").write(
        "seeds %s\ntime %d\nturns %d\njobs %d\nstarted %s\n" % (
            ",".join(map(str, seeds)), args.time, args.turns, jobs, time.ctime()))
    lock, progress = threading.Lock(), {"done": 0, "failed": 0, "total": len(todo)}
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for e in todo:
            pool.submit(play, ssbot, games, e, args.time, args.turns, override, lock, progress)
    report(ssbot, base, seeds, quiet=False)
    if progress["failed"]:
        print("\n%d episodes failed: run again to replay them (logs in games/)" % progress["failed"])
        sys.exit(1)


if __name__ == "__main__":
    main()
