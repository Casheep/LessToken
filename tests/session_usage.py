import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("session_usage", Path(__file__).resolve().parents[1] / "scripts/analyze_session_usage.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def usage(inputs=100, outputs=20, cached=60, reasoning=10):
    return dict(input_tokens=inputs, output_tokens=outputs, cached_input_tokens=cached,
                reasoning_output_tokens=reasoning, total_tokens=inputs + outputs)


class SessionUsageTests(unittest.TestCase):
    def test_authoritative_records_not_double_counted(self):
        records = [{"type": "turn_context", "payload": {"turn_id": "private-turn", "model": "same-model", "effort": "high"}},
                   {"type": "token_usage_record", "payload": {"turn_id": "private-turn", "response_id": "private-response", "usage": usage()}},
                   {"type": "event_msg", "payload": {"type": "token_count", "info": {"last_token_usage": usage(), "total_token_usage": usage()}}}]
        records.append(records[1])
        result = module.analyze(records)
        self.assertEqual(result["model_calls"], 1)
        self.assertEqual(result["totals"]["total_tokens"], 120)
        self.assertEqual(result["totals"]["uncached_input_tokens"], 40)
        self.assertEqual(result["totals"]["visible_output_tokens"], 10)
        self.assertNotIn("private-", json.dumps(result))

    def test_legacy_uses_last_not_cumulative(self):
        first = {"type": "event_msg", "payload": {"type": "token_count", "info": {"last_token_usage": usage(), "total_token_usage": usage()}}}
        second = {"type": "event_msg", "payload": {"type": "token_count", "info": {"last_token_usage": usage(), "total_token_usage": usage(200, 40)}}}
        result = module.analyze([first, first, second])
        self.assertEqual(result["model_calls"], 2)
        self.assertEqual(result["totals"]["total_tokens"], 240)

    def test_filter_and_equal_usage_calls(self):
        records = [{"type": "token_usage_record", "payload": {"turn_id": turn, "response_id": str(i), "usage": usage()}}
                   for i, turn in enumerate(("a", "b", "b"))]
        self.assertEqual(module.analyze(records)["model_calls"], 3)
        self.assertEqual(module.analyze(records, "b")["model_calls"], 2)

    def test_invalid_counters_fail_closed(self):
        for bad in (dict(usage(), input_tokens=-1), dict(usage(), cached_input_tokens=101),
                    dict(usage(), reasoning_output_tokens=21), dict(usage(), total_tokens=1),
                    dict(usage(), output_tokens=True), {}, None):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                module.validate_usage(bad)

    def test_partial_live_tail_and_malformed_complete_line(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "records.jsonl"
            path.write_text('{"type":"safe"}\n{"type":', encoding="utf-8")
            self.assertEqual(list(module.read_records(path)), [{"type": "safe"}])
            path.write_text('{"type":"safe"}\nINVALID\n', encoding="utf-8")
            with self.assertRaises(ValueError):
                list(module.read_records(path))


if __name__ == "__main__":
    unittest.main()
