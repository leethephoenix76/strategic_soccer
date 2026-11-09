"""Shared helpers of the server scripts (bench.py, reference.py): positions,
one ssbot search with a timeout, CSV tables that survive an interruption,
a description of the machine. docs/server_plan.md."""
import csv, json, os, platform, re, subprocess, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SSBOT = os.environ.get("SSBOT", os.path.join(ROOT, "build", "linux", "ssbot"))
DEFAULT_BOT = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-b8-t120"   # bot by default since 02.11.2026
RESULTS = os.environ.get("SS_RESULTS", os.path.join(ROOT, "results"))


def cases(selected=None, path=None):
    path = path or os.path.join(ROOT, "tests", "bot_suite_cases_v4.json")
    data = json.load(open(path, encoding="utf-8"))
    return [c for c in data["cases"] if not selected or c["id"] in selected]


def mover(position):
    """'w' or 'b': the side to move in a position string."""
    return position.split()[0]


def read_moves(path):
    """'case_id|actions' lines -> {case_id: actions}."""
    moves = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if "|" in line and not line.startswith("#"):
                k, v = line.rstrip("\n").split("|", 1)
                moves[k.strip()] = v.strip()
    return moves


PLAN = re.compile(r"plan (.*), value ([-\d.]+), eval ([-+\d.]+), depth (\d+), ([\d.]+) s")
EXPANSIONS = re.compile(r"(\d+) expansions")
ROOT_MOVE = re.compile(r"^\s+root \[([-\d.]+), ([-\d.]+)\] (.*)$")


def search(position, bot, depth=0, seconds=0.0, threads=1, then="", width=0, timeout=None,
           root_moves=False, extra=()):
    """One `ssbot search`. seconds 0 - no time limit (a fixed depth); timeout - the wall
    limit of the process (killed after it: status 'timeout'). Returns a dict of strings."""
    args = [SSBOT, "search", position, "--bot", bot, "--time", str(seconds), "--threads", str(threads)]
    if depth: args += ["--turn-depth", str(depth)]
    if then: args += ["--then", then]
    if width: args += ["--width", str(width)]
    if root_moves: args += ["--root-moves"]
    args += list(extra)
    start = time.time()
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        out, err, status = r.stdout, r.stderr, "ok" if r.returncode == 0 else "exit%d" % r.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        err, status = "", "timeout"
    wall = time.time() - start
    lines = out.strip().splitlines()
    m = None
    for line in reversed(lines):
        m = PLAN.search(line)
        if m: break
    note = next((l for l in lines if "expansions" in l), "")
    e = EXPANSIONS.search(note)
    if status == "ok" and not m: status = "noparse"
    row = {
        "status": status,
        "plan": m.group(1) if m else "", "value": m.group(2) if m else "", "eval": m.group(3) if m else "",
        "depth": m.group(4) if m else "", "seconds": m.group(5) if m else "", "wall": "%.2f" % wall,
        "expansions": e.group(1) if e else "",
        "error": "" if status == "ok" else (err.strip()[-200:] or (lines[-1][-200:] if lines else "")),
    }
    if root_moves:
        row["root_moves"] = [(float(a), float(b), act) for a, b, act in
                             (ROOT_MOVE.match(l).groups() for l in lines if ROOT_MOVE.match(l))]
    return row


def estimate_position(position, then):
    """The position after `then` (ssbot estimate prints it): to merge candidates that end
    in the same state. '' when it fails."""
    r = subprocess.run([SSBOT, "estimate", position, "--bot", DEFAULT_BOT, "--then", then],
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith("position "): return line[len("position "):].strip()
    return ""


class Table:
    """A CSV written row by row (flushed), with resume: rows whose key is already in the
    file are skipped, unless their status is in `redo`."""

    def __init__(self, path, fields, key, redo=()):
        self.path, self.fields, self.key = path, fields, key
        self.done = {}
        self.lock = threading.Lock()
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        if os.path.exists(path) and os.path.getsize(path) > 0:
            with open(path, newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            keep = [r for r in rows if r.get("status", "ok") not in redo]
            for r in keep: self.done[self.k(r)] = r
            if len(keep) != len(rows):   # rewrite without the rows to redo
                self._rewrite(keep)
        else:
            self._rewrite([])

    def k(self, row):
        return tuple(str(row.get(f, "")) for f in self.key)

    def _rewrite(self, rows):
        with open(self.path, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=self.fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)

    def has(self, **key):
        return self.k(key) in self.done

    def get(self, **key):
        return self.done.get(self.k(key))

    def add(self, row):
        with self.lock, open(self.path, "a", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=self.fields, extrasaction="ignore").writerow(row)
            self.done[self.k(row)] = row

    def rows(self):
        return list(self.done.values())


def cpu_info():
    """(logical, physical) cores this process may use: the CPUs of its affinity mask (online,
    allowed by the cgroup) and their distinct (core, socket) pairs from lscpu. os.cpu_count()
    is not used: on a machine with offline CPUs it counts them too (EPYC 7763: 128, 40 online)."""
    try:
        allowed = set(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        allowed = set(range(os.cpu_count() or 1))
    physical = set()
    try:
        out = subprocess.run(["lscpu", "-p=cpu,core,socket"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if not line or line.startswith("#"): continue
            cpu, core, socket = line.split(",")[:3]
            if int(cpu) in allowed: physical.add((core, socket))
    except (OSError, ValueError):
        pass
    return len(allowed), len(physical) or len(allowed)


def default_threads():
    logical, physical = cpu_info()
    ts = [t for t in (1, 2, 4, 8, 16, 24, 32) if t < physical] + [physical, logical]
    return sorted(set(ts))


def machine_info():
    logical, physical = cpu_info()
    info = {"host": platform.node(), "platform": platform.platform(), "python": platform.python_version(),
            "logical": logical, "physical": physical, "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    for name, cmd in (("lscpu", ["lscpu"]), ("memory", ["free", "-g"]), ("compiler", ["g++", "--version"]),
                      ("numa", ["numactl", "--hardware"]), ("load", ["uptime"])):
        try:
            info[name] = subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            info[name] = ""
    return info


def schedule(jobs, cores, fn, cost=None, max_threads=16):
    """Runs fn(job, threads) for every job on `cores` cores, the costliest first (cost(job): e.g. the
    seconds of the same search one level cheaper). A job gets threads in proportion to its share of the
    remaining cost (05.11: a few searches of hours among many of seconds got one thread each and ran
    alone for hours), at least one, at most max_threads (more did not help on the server, 03.11); when
    fewer jobs wait than cores are free, the free cores are shared among them. The value of a search
    does not depend on its threads, only its time."""
    jobs = sorted(jobs, key=cost, reverse=True) if cost else list(jobs)
    costs = [max(0.0, float(cost(j))) for j in jobs] if cost else [0.0] * len(jobs)
    total = sum(costs)
    cond = threading.Condition()
    free = [cores]
    workers = []
    def run(job, n):
        try:
            fn(job, n)
        finally:
            with cond:
                free[0] += n
                cond.notify_all()
    for i, job in enumerate(jobs):
        with cond:
            while free[0] < 1: cond.wait()
            waiting = len(jobs) - i
            by_cost = int(cores * costs[i] / total) if total > 0 else 1
            by_count = free[0] // waiting if waiting < free[0] else 1
            n = max(1, min(max_threads, free[0], max(by_cost, by_count)))
            free[0] -= n
        w = threading.Thread(target=run, args=(job, n), daemon=True)
        w.start()
        workers.append(w)
    for w in workers: w.join()
