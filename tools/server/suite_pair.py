#!/usr/bin/env python3
"""The fixed v4 suite for two bots at the same time budgets (07.11.2026).

    python3 tools/server/suite_pair.py --bots A,B --budgets 90,180 [--jobs N]

One process per search, one thread each; results go to $SS_RESULTS/suite_pair.csv
(resumable: rows already there are skipped)."""
import argparse
import json
import os

import sslib

FIELDS = ["case", "bot", "seconds_budget", "status", "plan", "value", "eval", "depth", "seconds", "wall",
          "expansions", "error"]


def main():
    _, physical = sslib.cpu_info()
    p = argparse.ArgumentParser()
    p.add_argument("--bots", required=True)
    p.add_argument("--budgets", default="90,180")
    p.add_argument("--cases", default=os.path.join(sslib.ROOT, "tests", "bot_suite_cases_v4.json"))
    p.add_argument("--jobs", type=int, default=physical)
    a = p.parse_args()
    cases = json.load(open(a.cases, encoding="utf-8"))["cases"]
    t = sslib.Table(os.path.join(sslib.RESULTS, "suite_pair.csv"), FIELDS, ["case", "bot", "seconds_budget"])
    todo = [(c, b, float(s)) for s in a.budgets.split(",") for b in a.bots.split(",") for c in cases
            if not t.has(case=c["id"], bot=b, seconds_budget=float(s))]
    print("%d searches" % len(todo), flush=True)

    def fn(item, n):
        c, b, s = item
        v = sslib.search(c["position"], b, seconds=s, threads=1, timeout=s * 4 + 120)
        t.add({"case": c["id"], "bot": b, "seconds_budget": s, **v})
        print(c["id"], b, s, v.get("plan", ""), v.get("eval", ""), flush=True)

    sslib.schedule(todo, a.jobs, fn, lambda item: item[2], max_threads=1)


if __name__ == "__main__":
    main()
