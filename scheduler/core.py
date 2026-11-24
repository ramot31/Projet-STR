"""Deterministic, one-CPU, discrete-millisecond RM/DM/EDF simulator.

Intervals use the half-open convention [start, end). A job may finish exactly
at its absolute deadline; it is then not a miss. Integer execution times are
intentional so every run is reproducible without floating-point scheduling
decisions.
"""

from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional


@dataclass(frozen=True)
class Task:
    task_id: str
    execution: int
    period: int
    deadline: Optional[int] = None
    release: int = 0
    base_priority: int = 0
    hard_real_time: bool = False
    risk: Optional[float] = None

    def __post_init__(self):
        if self.execution < 0 or self.period <= 0:
            raise ValueError("execution must be >= 0 and period must be > 0")
        if self.deadline is not None and self.deadline <= 0:
            raise ValueError("deadline must be > 0")
        if self.release < 0:
            raise ValueError("release must be >= 0")

    @property
    def relative_deadline(self) -> int:
        return self.period if self.deadline is None else self.deadline


@dataclass
class Job:
    task: Task
    release: int
    absolute_deadline: int
    sequence: int
    remaining: int = field(init=False)
    missed: bool = False

    def __post_init__(self):
        self.remaining = self.task.execution


@dataclass
class ScheduleResult:
    algorithm: str
    horizon: int
    jobs: list[Job]
    timeline: list[tuple[int, int, str]]
    deadline_misses: list[Job]

    @property
    def miss_count(self) -> int:
        return len(self.deadline_misses)

    @property
    def schedulable(self) -> bool:
        return self.miss_count == 0

    def intervals(self) -> list[tuple[int, int, str]]:
        """Merge adjacent execution ticks belonging to the same job."""
        merged: list[tuple[int, int, str]] = []
        for start, end, label in self.timeline:
            if merged and merged[-1][1] == start and merged[-1][2] == label:
                merged[-1] = (merged[-1][0], end, label)
            else:
                merged.append((start, end, label))
        return merged


def hyperperiod(tasks: Iterable[Task]) -> int:
    from math import gcd
    value = 1
    for task in tasks:
        value = value * task.period // gcd(value, task.period)
    return value


def _priority_function(algorithm: str, ai_priority: Optional[Callable[[Job], float]]):
    algorithm = algorithm.upper()
    if algorithm not in {"RM", "DM", "EDF"}:
        raise ValueError("algorithm must be RM, DM, or EDF")

    def key(job: Job):
        # Hard real-time work is protected from AI reordering. For RM/DM it
        # keeps the classical fixed key; for EDF the absolute deadline wins.
        if algorithm == "RM":
            classical = (job.task.period, job.task.task_id)
        elif algorithm == "DM":
            classical = (job.task.relative_deadline, job.task.task_id)
        else:
            classical = (job.absolute_deadline, job.task.task_id)
        if job.task.hard_real_time:
            return (0, classical, job.release, job.sequence)
        boost = ai_priority(job) if ai_priority else 0.0
        # Negative boost makes lower numbers win, while classical fields keep
        # ties deterministic and preserve the selected scheduler's semantics.
        return (1, -boost, classical, job.release, job.sequence)

    return key


def simulate(tasks: Iterable[Task], horizon: int, algorithm: str = "RM",
             ai_priority: Optional[Callable[[Job], float]] = None) -> ScheduleResult:
    """Run a preemptive discrete-time simulation on one CPU."""
    task_list = list(tasks)
    if horizon <= 0:
        raise ValueError("horizon must be > 0")
    key = _priority_function(algorithm, ai_priority)
    jobs: list[Job] = []
    for task in task_list:
        release = task.release
        sequence = 0
        while release < horizon:
            jobs.append(Job(task, release, release + task.relative_deadline, sequence))
            sequence += 1
            release += task.period
    jobs.sort(key=lambda job: (job.release, job.task.task_id, job.sequence))
    misses: list[Job] = []
    timeline: list[tuple[int, int, str]] = []
    for now in range(horizon):
        for job in jobs:
            if not job.missed and job.remaining > 0 and job.absolute_deadline <= now:
                job.missed = True
                misses.append(job)
        ready = [job for job in jobs if job.release <= now and job.remaining > 0]
        if not ready:
            timeline.append((now, now + 1, "IDLE"))
            continue
        selected = min(ready, key=key)
        selected.remaining -= 1
        timeline.append((now, now + 1, selected.task.task_id))
    # A horizon may end before a job's deadline. Count only deadlines reached
    # in the simulated window; this makes finite-window experiments fair.
    for job in jobs:
        if not job.missed and job.remaining > 0 and job.absolute_deadline <= horizon:
            job.missed = True
            misses.append(job)
    return ScheduleResult(algorithm.upper(), horizon, jobs, timeline, misses)
