import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ai"))

import node_b
import predict


class ModelInputContractTests(unittest.TestCase):
    def setUp(self):
        node_b.CONTEXT.clear()
        self.addCleanup(node_b.CONTEXT.clear)

    def test_existing_protocol_delay_reaches_model(self):
        with patch.object(node_b, "predict", return_value={"risk": 0.5}) as inference, \
             patch.object(node_b, "send", return_value=True) as send:
            node_b.on_flight_delay({
                "flight": "TU701", "delay": "10", "connecting_pax": "12", "src_ts": "123"
            })
        features = inference.call_args.args[0]
        self.assertEqual(features["delay_min"], "10")
        self.assertEqual(features["connecting_pax"], "12")
        self.assertNotIn("delay", features)
        self.assertEqual(send.call_args.args[2], "flight=TU701,risk=0.50,src_ts=123")

    def test_predict_rejects_non_model_names(self):
        for name in ("nb_connecting", "rush_hour", "time_left_min", "delay", "typo"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                predict.predict({"delay_min": 10, "scheduled_buffer_min": 55, name: 1})

    def test_duplicate_delay_and_unknown_context_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "delay"):
            node_b.on_flight_delay({"flight": "TU701", "delay": "10", "delay_min": "11"})
        with self.assertRaisesRegex(ValueError, "rush_hour"):
            node_b.update_context({"rush_hour": "1"})

    def test_queue_protocol_and_model_context_remain_supported(self):
        node_b.update_context({"queue": "100", "factor": "2", "peak_hour": "1"})
        self.assertEqual(node_b.CONTEXT["security_wait_min"], 20.0)
        self.assertEqual(node_b.CONTEXT["peak_hour"], "1")


if __name__ == "__main__":
    unittest.main()
