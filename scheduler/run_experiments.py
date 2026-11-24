"""Reproducible Member 5 experiments and chart generation."""

import argparse
import csv
import random
from pathlib import Path

from .ai_policy import AIPolicy, RiskRecord
from .core import Task, hyperperiod, simulate
from .plotting import gantt, miss_plot

ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"


def manual_tasks():
    return [Task("T1", 5, 20), Task("T2", 10, 50), Task("T3", 20, 100)]


def load_tasks(target: float, seed: int = 42):
    """Create integer-C tasks with deterministic utilization near target."""
    periods = [20, 25, 40, 50, 100]
    rng = random.Random(seed + round(target * 1000))
    # Equal shares of utilization produce C_i ~= target*T_i/n. A tiny,
    # seeded perturbation makes the task sets distinct while the tuning loop
    # keeps the reported utilization close to the requested level.
    cs = [max(1, round(target * period / len(periods) + rng.choice([-1, 0, 0, 1])))
          for period in periods]
    while sum(c / p for c, p in zip(cs, periods)) < target - 0.025:
        i = max(range(len(periods)), key=lambda j: 1 / periods[j])
        cs[i] += 1
    while sum(c / p for c, p in zip(cs, periods)) > target + 0.025:
        candidates = [i for i, c in enumerate(cs) if c > 1]
        if not candidates:
            break
        i = min(candidates, key=lambda j: 1 / periods[j])
        cs[i] -= 1
    return [Task(f"L{i + 1}", c, p) for i, (c, p) in enumerate(zip(cs, periods))]


def run_load(levels=None):
    levels = levels or [round(0.8 + i * 0.1, 2) for i in range(5)]
    rows = []
    for target in levels:
        tasks = load_tasks(target)
        actual = sum(task.execution / task.period for task in tasks)
        horizon = hyperperiod(tasks)
        row = {"requested_utilization": target, "utilization": actual, "horizon": horizon}
        for algorithm in ("RM", "DM", "EDF"):
            row[algorithm + "_misses"] = simulate(tasks, horizon, algorithm).miss_count
        rows.append(row)
    return rows


def run_ai_comparison():
    risks = {"TU701": 0.87, "TU703": 0.65, "TU702": 0.20}
    records = {key: RiskRecord(key, value, 0) for key, value in risks.items()}
    tasks = [Task("TU701", 4, 20, 20), Task("TU703", 3, 20, 20), Task("TU702", 2, 20, 20)]
    policy = AIPolicy()
    fixed = simulate(tasks, 20, "DM")
    guided = simulate(tasks, 20, "DM", policy.selector(records))
    return fixed, guided, policy, risks


def write_csv(rows, path):
    with Path(path).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=RESULTS)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tasks = manual_tasks()
    horizon = hyperperiod(tasks)
    for algorithm in ("RM", "DM", "EDF"):
        result = simulate(tasks, horizon, algorithm)
        gantt(result, args.output / f"manual_{algorithm.lower()}.png",
              f"Manual task set — {algorithm} (U=0.65)")
    rows = run_load()
    write_csv(rows, args.output / "load_experiment.csv")
    miss_plot(rows, args.output / "load_deadline_misses.png")
    fixed, guided, policy, risks = run_ai_comparison()
    write_csv([
        {"policy": "fixed", "misses": fixed.miss_count, "order": ">".join(x[2] for x in fixed.intervals() if x[2] != "IDLE")},
        {"policy": "ai_guided", "misses": guided.miss_count, "order": ">".join(x[2] for x in guided.intervals() if x[2] != "IDLE")},
    ], args.output / "ai_policy_comparison.csv")
    print(f"Generated charts and tables in {args.output}")
    print(f"AI thresholds: low<{policy.low_threshold}, medium<{policy.critical_threshold}, critical>=threshold")
    print("AI risks:", risks)


if __name__ == "__main__":
    main()
