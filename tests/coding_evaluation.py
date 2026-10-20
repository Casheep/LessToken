import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

COMPILER = str(Path(sys.argv.pop(1)).resolve())
spec = importlib.util.spec_from_file_location("coding_evaluation", Path(__file__).resolve().parents[1] / "scripts/coding_evaluation.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CodingEvaluationTests(unittest.TestCase):
    def test_accepts_working_fixes_and_rejects_original_bugs(self):
        with tempfile.TemporaryDirectory(prefix="lesstoken-grader-") as temp:
            for case in module.CASES:
                with self.subTest(case=case["id"]):
                    good = module.grade(case["id"], {"code": case["reference"]}, COMPILER, Path(temp)/case["id"]/"good")
                    self.assertTrue(good["passed"], good)
                    bad = module.grade(case["id"], {"code": case["broken"]}, COMPILER, Path(temp)/case["id"]/"bad")
                    self.assertFalse(bad["passed"], bad)

    def test_invalid_answers_do_not_compile(self):
        for answer in ({"code": 4}, {"code": "", "other": 1}, {}, None):
            result = module.grade("safe-pagination", answer, COMPILER, "must-not-be-created")
            self.assertEqual(result, {"passed": False, "phase": "answer-format"})


if __name__ == "__main__":
    unittest.main()
