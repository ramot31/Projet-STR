"""Configurable AI risk-to-priority adapter.

Risk is advisory only. Hard-real-time jobs are protected by ``core.simulate``
and never use the AI boost when their priority key is built.
"""

from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class RiskRecord:
    flight_id: str
    risk: Optional[float]
    base_priority: float = 0.0
    hard_real_time: bool = False


@dataclass(frozen=True)
class AIPolicy:
    low_threshold: float = 0.35
    medium_threshold: float = 0.65
    critical_threshold: float = 0.85
    boost_factor: float = 3.0
    max_boost: float = 3.0
    unavailable_risk: float = 0.0

    def __post_init__(self):
        if not 0 <= self.low_threshold <= self.medium_threshold <= self.critical_threshold <= 1:
            raise ValueError("risk thresholds must be ordered within [0, 1]")

    def level(self, risk: Optional[float]) -> str:
        value = self.unavailable_risk if risk is None else min(1.0, max(0.0, risk))
        if value >= self.critical_threshold:
            return "critical"
        if value >= self.low_threshold:
            return "medium"
        return "low"

    def priority(self, record: RiskRecord) -> float:
        """Higher values are selected first among advisory jobs."""
        if record.hard_real_time:
            return record.base_priority
        risk = self.unavailable_risk if record.risk is None else min(1.0, max(0.0, record.risk))
        return record.base_priority + min(self.max_boost, risk * self.boost_factor)

    def selector(self, records: dict[str, RiskRecord]):
        return lambda job: self.priority(records.get(
            job.task.task_id,
            RiskRecord(job.task.task_id, job.task.risk, job.task.base_priority, job.task.hard_real_time),
        ))
