import unittest

from scheduler.ai_policy import AIPolicy, RiskRecord
from scheduler.core import Task, hyperperiod, simulate


class SchedulerTests(unittest.TestCase):
    def test_manual_utilization_and_hyperperiod(self):
        tasks = [Task("T1", 5, 20), Task("T2", 10, 50), Task("T3", 20, 100)]
        self.assertAlmostEqual(sum(t.execution / t.period for t in tasks), 0.65)
        self.assertEqual(hyperperiod(tasks), 100)

    def test_rm_dm_edf_are_reproducible(self):
        tasks = [Task("A", 2, 5), Task("B", 1, 7, 4)]
        for algorithm in ("RM", "DM", "EDF"):
            first = simulate(tasks, 35, algorithm)
            second = simulate(tasks, 35, algorithm)
            self.assertEqual(first.timeline, second.timeline)
            self.assertEqual(first.miss_count, second.miss_count)

    def test_deadline_less_than_period_and_miss(self):
        result = simulate([Task("A", 4, 10, 3)], 10, "DM")
        self.assertEqual(result.miss_count, 1)

    def test_ai_policy_threshold_boundaries_fallback_and_cap(self):
        policy = AIPolicy()
        self.assertEqual(policy.level(0.349999), "low")
        self.assertEqual(policy.level(0.35), "medium")
        self.assertEqual(policy.level(0.87), "critical")
        self.assertEqual(policy.level(0.65), "medium")
        self.assertEqual(policy.level(0.85), "critical")
        self.assertEqual(policy.level(None), "low")
        self.assertEqual(policy.priority(RiskRecord("x", 1.0)), 3.0)

    def test_ai_changes_advisory_order_but_protects_critical_work(self):
        policy = AIPolicy()
        advisory_records = {
            "A": RiskRecord("A", 0.1, 0.0),
            "B": RiskRecord("B", 0.9, 0.0),
        }
        advisory_tasks = [Task("A", 1, 10), Task("B", 1, 10)]
        fixed = simulate(advisory_tasks, 2, "DM")
        guided = simulate(advisory_tasks, 2, "DM", policy.selector(advisory_records))
        self.assertEqual(fixed.timeline[0][2], "A")
        self.assertEqual(guided.timeline[0][2], "B")
        self.assertNotEqual(fixed.timeline, guided.timeline)

        protected_records = {
            "critical": RiskRecord("critical", 0.0, 0.0, hard_real_time=True),
            "advisory": RiskRecord("advisory", 1.0, 0.0),
        }
        protected_tasks = [Task("critical", 1, 10, hard_real_time=True), Task("advisory", 1, 10)]
        protected = simulate(protected_tasks, 2, "DM", policy.selector(protected_records))
        self.assertEqual(protected.timeline[0][2], "critical")


if __name__ == "__main__":
    unittest.main()
