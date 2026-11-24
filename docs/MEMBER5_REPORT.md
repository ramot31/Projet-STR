# Member 5 — Scheduling study and AI-guided policy

## Scope and assumptions

The standalone simulator models one preemptive CPU, independent periodic
tasks, zero context-switch cost, integer millisecond execution, and
deterministic tie-breaking. It is an experiment model, not a replacement for
Node C's networking or the POSIX-thread implementation owned by another
member. Deadline misses are counted per released job whose execution remains
unfinished when its absolute deadline is reached.

## Manual task set

For T1=(C=5,T=20), T2=(10,50), and T3=(20,100):

`U = 5/20 + 10/50 + 20/100 = 0.25 + 0.20 + 0.20 = 0.65`.

The hyperperiod is `LCM(20,50,100)=100 ms`. The generated RM, DM, and EDF
timelines are `scheduler/results/manual_rm.png`, `manual_dm.png`, and
`manual_edf.png`; regenerate them with `python -m scheduler.run_experiments`.
With the usual constrained-deadline, independent-task assumptions, the RM
sufficient bound for three tasks is `3(2^(1/3)-1) ≈ 0.7798`; 0.65 is below it.
EDF is schedulable on one processor under the corresponding standard model if
`U <= 1` (and the task deadlines/initial-release assumptions are respected).
These are sufficient conditions, not a claim that utilization alone proves
every arbitrary task set schedulable.

## Load experiment

The experiment deterministically creates task sets near U=0.8, 0.9, 1.0, 1.1,
and 1.2, runs all three algorithms through the actual simulator, and records
misses in `scheduler/results/load_experiment.csv`. The plot is generated from
that CSV data; no results are manually inserted. RM can fail while EDF still
succeeds because RM is a fixed-priority assignment and its utilization bound
is sufficient but not necessary. Above U=1, a single processor cannot execute
all work under the standard assumptions, so misses must eventually occur.

## AI-guided policy

The policy uses `priority = base_priority + min(3, risk * 3)` for advisory
work. With the default configuration, low risk is `[0.0, 0.35)`, medium risk
is `[0.35, 0.85)`, and critical risk is `[0.85, 1.0]`; therefore TU702=20% is
low, TU703=65% is medium, and TU701=87% is critical. Missing or unavailable
risk maps to 0.0. Ties are resolved by the scheduler's deterministic
classical key. AI priority is only an advisory ordering signal: hard-real-time
jobs bypass the boost and retain the classical RM/DM/EDF key. A prediction
never guarantees a deadline.

The comparison uses TU701=87%, TU703=65%, and TU702=20% and writes its actual
baseline/guided simulation results to `scheduler/results/ai_policy_comparison.csv`.
