"""Native regression tests. All data and state stay in a temporary directory."""
import json
import os
from pathlib import Path
import re
import random
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())


class OptimizationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lesstoken-opt-")
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, LESSTOKEN_DATA=str(self.root / "state"))

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text):
        path = self.root / name
        path.write_bytes(text.encode("utf-8"))
        return path

    def run_cli(self, *args, text=None, ok=True):
        result = subprocess.run([EXE, *args], input=text, text=True, encoding="utf-8",
                                capture_output=True, env=self.env, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def search(self, **args):
        args.setdefault("query", "TARGET")
        args.setdefault("path", "code.cpp")
        result = self.run_cli("ops", "--root", str(self.root),
                              text=json.dumps({"tool": "lt_search", "arguments": args}))
        return json.loads(result.stdout)

    def code(self, hits=(10, 13), count=60):
        rows = [f"original source line {i}" + (" TARGET" if i in hits else "")
                for i in range(1, count + 1)]
        self.write("code.cpp", "\n".join(rows) + "\n")
        return rows

    def test_exact_union_and_hit_positions(self):
        rows = self.code()
        out = self.search(before=4, after=7)
        self.assertTrue(out["complete"])
        self.assertEqual(out["matching_lines"], 2)
        self.assertEqual(len(out["matches"]), 1)
        item = out["matches"][0]
        self.assertEqual(item["lines"], [10, 13])
        self.assertEqual(item["context"], "\n".join(f"{i}: {rows[i-1]}" for i in range(6, 21)))
        self.assertNotIn("clipped", item)

    def test_default_window_compatibility(self):
        rows = self.code(hits=(10,))
        item = self.search()["matches"][0]
        self.assertEqual(item["context"], "\n".join(f"{i}: {rows[i-1]}" for i in range(9, 13)))

    def test_zero_window_has_only_matches(self):
        self.code(hits=(10, 13))
        out = self.search(before=0, after=0)
        self.assertEqual(len(out["matches"]), 2)
        self.assertTrue(all(len(x["context"].splitlines()) == 1 for x in out["matches"]))

    def test_file_edges_and_no_final_newline(self):
        self.write("code.cpp", "TARGET first\nmiddle\nTARGET last")
        out = self.search(before=40, after=40)
        self.assertTrue(out["complete"])
        self.assertEqual(out["matches"][0]["context"], "1: TARGET first\n2: middle\n3: TARGET last")

    def test_gap_and_file_boundaries_never_merge(self):
        self.code(hits=(5, 50))
        self.write("other.cpp", "TARGET other file\n")
        out = self.search(path=".", glob=["*.cpp"], before=2, after=2)
        self.assertEqual(len(out["matches"]), 3)
        self.assertEqual(out["matching_files"], 2)

    def test_match_limit_remains_explicit(self):
        self.code()
        out = self.search(before=4, after=7, limit=1)
        self.assertFalse(out["complete"])
        self.assertTrue(out["scan_complete"])
        self.assertEqual(out["matching_lines"], 2)

    def test_validation(self):
        self.code()
        for value in (-1, 1.5, "4", None, True):
            with self.subTest(value=value):
                result = self.run_cli("ops", "--root", str(self.root), ok=False,
                                      text=json.dumps({"tool": "lt_search", "arguments": {
                                          "query": "TARGET", "before": value}}))
                self.assertNotEqual(result.returncode, 0)
        result = self.run_cli("ops", "--root", str(self.root), ok=False,
                              text=json.dumps({"tool": "lt_search", "arguments": {
                                  "query": "TARGET", "mode": "count", "after": 2}}))
        self.assertNotEqual(result.returncode, 0)

    def test_path_escape_rejected(self):
        result = self.run_cli("ops", "--root", str(self.root), ok=False,
                              text=json.dumps({"tool": "lt_search", "arguments": {
                                  "query": "TARGET", "path": "../", "after": 20}}))
        self.assertNotEqual(result.returncode, 0)

    def test_long_context_marked_clipped(self):
        self.write("code.cpp", ("x" * 1600 + "\n") * 20 + "TARGET\n" + ("y" * 1600 + "\n") * 20)
        out = self.search(before=40, after=40)
        self.assertTrue(out["matches"][0]["clipped"])
        self.assertLessEqual(len(out["matches"][0]["context"].encode()), 12000)

    def test_long_neighbor_and_deep_match_marked_clipped(self):
        for text in ("x" * 2000 + "\nTARGET\n", "TARGET\n" + "x" * 2000,
                     "x" * 70000 + " TARGET\n"):
            with self.subTest(size=len(text)):
                self.write("code.cpp", text)
                out = self.search(before=1, after=2)
                self.assertEqual(out["matching_lines"], 1)
                self.assertTrue(out["matches"][0]["clipped"])
                self.assertIn("TARGET", out["matches"][0]["context"])

    def test_utf8_source_is_exact(self):
        rows = ["// 中文注释", "auto TARGET = u8\"你好🙂\";", "// 后面的条件不能丢失"]
        self.write("code.cpp", "\n".join(rows))
        out = self.search(before=1, after=1)
        self.assertEqual(out["matches"][0]["context"], "\n".join(f"{i}: {s}" for i, s in enumerate(rows, 1)))

    def test_search_is_fresh_after_edit(self):
        self.write("code.cpp", "TARGET old\n")
        self.assertIn("old", self.search()["matches"][0]["context"])
        self.write("code.cpp", "TARGET new\n")
        self.assertIn("new", self.search()["matches"][0]["context"])

    def test_tool_discovery_exposes_bounds(self):
        out = self.run_cli("mcp", "--root", str(self.root), text=json.dumps({
            "jsonrpc": "2.0", "id": 1, "method": "tools/list"}))
        tools = json.loads(out.stdout)["result"]["tools"]
        search = next(t for t in tools if t["name"] == "lt_search")
        self.assertIn("40", search["inputSchema"]["properties"]["before"]["description"])
        self.assertNotIn("lt_batch", [t["name"] for t in tools])

    def reduce(self, raw, kind="log"):
        path = self.write("input.txt", raw)
        return self.run_cli("reduce", "--kind", kind, "--file", str(path)).stdout

    def test_repeated_block_order_and_multiplicity(self):
        block = "INFO stage fetch completed for local workspace\nINFO stage parse completed for local workspace\nINFO stage index completed for local workspace\n"
        raw = "BEGIN run\n" + block * 20 + "ERROR unique failure expected=4 actual=0\nEND run\n"
        out = self.reduce(raw)
        self.assertIn("[previous 3 lines repeated 19 more times]", out)
        self.assertIn("lossless=true",out)
        self.assertIn("ERROR unique failure expected=4 actual=0", out)
        expanded = re.sub(r"\[previous (\d+) lines repeated (\d+) more times\]\n",
                          lambda m: block * int(m[2]), out.split("\n[LessToken")[0])
        self.assertEqual(expanded, raw)
        ref = re.search(r"ref=([0-9a-f]{32})", out)[1]
        self.assertEqual((self.root / "state/results" / (ref + ".txt")).read_bytes(), raw.encode())

    def test_diagnostic_blocks_stay_expanded(self):
        for word in ("ERROR", "WARNING", "expected", "实际", "异常", "traceback"):
            with self.subTest(word=word):
                raw = (f"{word} important detail\nINFO unique context\n") * 20
                self.assertEqual(self.reduce(raw), raw)

    def test_near_duplicates_and_separated_blocks_stay_expanded(self):
        raw = "".join(f"INFO stage begin {i}\nINFO stage end {i}\n" for i in range(30))
        self.assertEqual(self.reduce(raw), raw)
        block = "INFO first stage with useful details\nINFO second stage with useful details\n"
        raw = "".join(block + f"INFO changing event {i}\n" for i in range(20))
        self.assertEqual(self.reduce(raw), raw)

    def test_raw_code_and_json_are_not_summarized(self):
        raw = ("void first() {}\nvoid second() {}\n") * 20
        self.assertEqual(self.reduce(raw, "raw"), raw)
        self.assertEqual(self.reduce(raw, "code"), raw)
        data = '[\n {"x": 1.000, "x": 2, "s": "中文 a b"}\n]' * 1
        self.assertEqual(self.reduce(data, "json"), data)

    def test_random_exact_block_reconstruction(self):
        rng = random.Random(20261020)
        for sample in range(40):
            width, repeats = rng.randint(2, 16), rng.randint(2, 12)
            block = "".join(f"INFO local stage {sample}-{i}: completed operation details\n" for i in range(width))
            raw = "BEGIN unique run\n" + block * repeats + "END unique run\n"
            out = self.reduce(raw)
            view = out.split("\n[LessToken")[0]
            # Decode the format generically; do not assume which block width won.
            expanded = []
            for line in view.splitlines():
                marker = re.fullmatch(r"\[previous (\d+) lines repeated (\d+) more times\]", line)
                if marker:
                    expanded.extend(expanded[-int(marker[1]):] * int(marker[2]))
                else:
                    expanded.append(line)
            self.assertEqual("\n".join(expanded) + "\n", raw)

    def test_process_runs_once_preserves_stderr_and_exit_status(self):
        self.write("job.py", "from pathlib import Path\nimport sys\n"
                   "p=Path(__file__).with_suffix('.count')\n"
                   "p.write_text(str(int(p.read_text())+1) if p.exists() else '1')\n"
                   "print(('INFO first local stage completed\\nINFO second local stage completed\\n')*30)\n"
                   "print('ERROR stderr stays complete',file=sys.stderr)\n"
                   "sys.exit(7)\n")
        result = self.run_cli("run", "--kind", "log", "--", sys.executable,
                              str(self.root / "job.py"), ok=False)
        self.assertEqual(result.returncode, 7)
        self.assertEqual((self.root / "job.count").read_text(), "1")
        self.assertIn("ERROR stderr stays complete", result.stderr)
        self.assertIn("process_exit_code=7", result.stdout)

    def test_reduction_failure_returns_complete_original(self):
        blocked = self.write("not-a-directory", "occupied")
        self.env["LESSTOKEN_DATA"] = str(blocked)
        raw = ("INFO first local stage completed\nINFO second local stage completed\n") * 30
        self.assertEqual(self.reduce(raw), raw)

    def test_oversized_search_preview_is_explicit_and_complete_counts_survive(self):
        self.code(hits=tuple(range(1,201)),count=200)
        result=self.search(limit=1000,before=65,after=55)
        self.assertEqual(result["matching_lines"],200)
        self.assertFalse(result["complete"])
        self.assertFalse(result["context_complete"])
        self.assertEqual(result["before"],40)
        self.assertEqual(result["after"],40)
        self.assertEqual(result["limit_capped_from"],1000)
        self.assertEqual(result["after_capped_from"],55)

    def test_large_table_page_request_preserves_pagination(self):
        self.write("rows.json",json.dumps([{"id":i} for i in range(600)]))
        def call(arguments):
            return json.loads(self.run_cli("ops","--root",str(self.root),text=json.dumps({"tool":"lt_result","arguments":arguments})).stdout)
        first=call({"action":"put","path":"rows.json","limit":1000,"max_bytes":16000})
        self.assertEqual(first["limit_capped_from"],1000)
        self.assertEqual(len(first["items"]),512)
        self.assertFalse(first["complete"])
        rest=call({"ref":first["ref"],"offset":first["next_offset"],"limit":1000})
        self.assertTrue(rest["complete"])
        self.assertEqual([item["id"] for item in first["items"]+rest["items"]],list(range(600)))


if __name__ == "__main__":
    unittest.main()
