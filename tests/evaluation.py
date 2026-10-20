import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("evaluate_codex", Path(__file__).resolve().parents[1] / "scripts/evaluate_codex.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EvaluationTests(unittest.TestCase):
    def transcript(self, answer, usage=None, extra=None):
        records = [{"type": "item.completed", "item": {"type": "agent_message", "text": json.dumps(answer)}},
                   {"type": "turn.completed", "usage": usage or {"input_tokens": 100, "output_tokens": 20,
                       "cached_input_tokens": 60, "reasoning_output_tokens": 5}}]
        return "\n".join(json.dumps(item) for item in records + (extra or []))

    def test_terminal_usage_and_exact_answer(self):
        result = module.parse_events(self.transcript({"ids": [13, 26]}), {"ids": [13, 26]})
        self.assertTrue(result["valid"] and result["passed"])
        self.assertEqual(result["usage"]["total_tokens"], 120)

    def test_failed_answer_is_not_invalidated_away(self):
        result = module.parse_events(self.transcript({"ids": [13]}), {"ids": [13, 26]})
        self.assertTrue(result["valid"])
        self.assertFalse(result["passed"])

    def test_integer_bool_confusion_is_rejected(self):
        self.assertFalse(module.parse_events(self.transcript({"count": True}), {"count": 1})["passed"])
        self.assertFalse(module.parse_events(self.transcript({"count": 1.0}), {"count": 1})["passed"])

    def test_missing_terminal_or_failed_turn_cannot_pass(self):
        for text in ('{"type":"turn.completed"}', '{"type":"item.completed","item":{"type":"agent_message","text":"{}"}}',
                     self.transcript({}, extra=[{"type": "turn.failed"}])):
            self.assertFalse(module.parse_events(text, {})["valid"])

    def test_missing_or_inconsistent_usage_cannot_pass(self):
        valid = {"input_tokens": 100, "output_tokens": 20, "cached_input_tokens": 60, "reasoning_output_tokens": 5}
        for counts in ({"input_tokens": 100, "output_tokens": 20}, dict(valid, cached_input_tokens=101),
                       dict(valid, reasoning_output_tokens=21), dict(valid, output_tokens=-1)):
            self.assertFalse(module.parse_events(self.transcript({}, usage=counts), {})["valid"])

    def test_multiple_terminal_usage_records_sum(self):
        text = self.transcript({}) + '\n' + json.dumps({"type": "turn.completed", "usage": {
            "input_tokens": 200, "output_tokens": 10, "cached_input_tokens": 80, "reasoning_output_tokens": 0}})
        self.assertEqual(module.parse_events(text, {})["usage"]["total_tokens"], 330)

    def test_failure_keeps_observed_cost(self):
        result = module.parse_events(self.transcript({}, extra=[{"type": "turn.failed"}]), {})
        self.assertFalse(result["valid"])
        self.assertEqual(result["observed_usage"]["total_tokens"], 120)
        result = module.parse_events(self.transcript({}) + '\nINVALID', {})
        self.assertFalse(result["valid"])
        self.assertEqual(result["observed_usage"]["total_tokens"], 120)

    def pair(self, case="a", before=100, after=50, before_pass=True, after_pass=True):
        return [{"case": case, "arm": arm, "valid": True, "passed": passed, "usage": {"total_tokens": total}}
                for arm, total, passed in (("baseline", before, before_pass), ("candidate", after, after_pass))]

    def test_no_quality_tradeoff_to_hit_target(self):
        result = module.compare_pairs(self.pair(after_pass=False), [{"id": "a"}])
        self.assertFalse(result["pilot_gate_passed"])
        self.assertEqual(result["regressions"], ["a"])
        self.assertEqual(result["reduction_percent"], 50)

    def test_no_passing_subset_or_average_of_percentages(self):
        results = self.pair() + self.pair("b", before=1000, after=1000, after_pass=False)
        report = module.compare_pairs(results, [{"id": "a"}, {"id": "b"}])
        self.assertEqual(report["reduction_percent"], 4.55)
        self.assertFalse(report["pilot_gate_passed"])
        self.assertFalse(module.compare_pairs(results[:-1], [{"id": "a"}, {"id": "b"}])["conclusive"])

    def test_duplicate_results_and_pilot_scope(self):
        pair = self.pair()
        with self.assertRaises(ValueError):
            module.compare_pairs(pair + pair, [{"id": "a"}])
        result = module.compare_pairs(pair, [{"id": "a"}])
        self.assertTrue(result["pilot_gate_passed"])
        self.assertFalse(result["goal_verified"])


if __name__ == "__main__":
    unittest.main()
