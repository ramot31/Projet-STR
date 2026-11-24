# Member 5 scheduler

This module is a standalone, deterministic, one-CPU simulator. It uses
discrete one-millisecond ticks and half-open execution intervals `[start,end)`.
At each tick, releases are materialized, overdue unfinished jobs are recorded
as deadline misses, and one ready job is selected. A job completing at its
deadline is successful. Ties are resolved by task identifier, release time,
then release sequence.

Algorithms:

- RM: shortest period first.
- DM: shortest relative deadline first, including `D < T`.
- EDF: earliest absolute deadline first.

Run from the repository root:

```text
python -m scheduler.run_experiments
python -m unittest scheduler.test_scheduler
```

Outputs are written only to `scheduler/results/`: three manual Gantt charts,
the CPU-load CSV/plot, and the fixed-vs-AI-guided comparison CSV.
