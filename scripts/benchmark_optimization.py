"""Compare two native builds on a fixed, checked workload; no model/API calls.

Requires tiktoken in the development environment only. Output measures tool
payloads, not provider billing, hidden reasoning, chat history or subscription.
Source fixtures come from an explicit Git revision, never changing working files.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--source-ref", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    import tiktoken
    encodings = {name: tiktoken.get_encoding(name) for name in ("o200k_base", "cl100k_base")}
    repo = Path(__file__).resolve().parents[1]
    executables = {name: str(Path(path).resolve()) for name, path in (
        ("baseline", args.baseline), ("candidate", args.candidate))}
    commit = subprocess.check_output(["git", "rev-parse", "--verify", args.source_ref + "^{commit}"],
                                     cwd=repo, text=True).strip()
    results = {"source_commit": commit, "tiktoken_version": importlib.metadata.version("tiktoken"),
               "scope": "Tool call JSON + returned text, and one copy of tool definitions per workload. "
                        "Excludes model reasoning, repeated history, host envelopes, skills and billing. "
                        "Logs/data controls are synthetic; source is frozen at source_commit. No API calls. "
                        "raw_reference reuses baseline calls/schema, substitutes original whole-file text for "
                        "log/test/JSON results, and keeps baseline source/code results. It is a text-volume "
                        "reference, not a measured run with the plugin uninstalled.",
               "binaries_sha256": {k: hashlib.sha256(Path(v).read_bytes()).hexdigest()
                                   for k, v in executables.items()}, "fixtures_sha256": {}, "cases": []}
    with tempfile.TemporaryDirectory(prefix="lesstoken-benchmark-") as temp:
        root = Path(temp)
        for relative in ("src/core.cpp", "src/query.cpp", "src/main.cpp", "src/storage.cpp"):
            data = subprocess.check_output(["git", "show", f"{commit}:{relative}"], cwd=repo)
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        block = ("INFO local worker: opened configured input stream\n"
                 "INFO local worker: parsed complete input records\n"
                 "INFO local worker: committed index checkpoint\n")
        repeated = "BEGIN local indexing\n" + block * 40 + "ERROR unique checkpoint mismatch expected=8 actual=3\nEND local indexing\n"
        status = ("INFO monitor: local queue scan completed\n"
                  "INFO monitor: no pending work items\n"
                  "INFO monitor: next scheduled check in 10 seconds\n") * 25
        unique = "".join(f"INFO compiling source unit {i}: completed phase {i % 7}, checksum {i * 7919:08x}\n" for i in range(70))
        tests = "".join(f"tests/test_component.py::test_case_{i:03} PASSED [local fixture execution]\n" for i in range(120))
        tests += "FAIL test_boundary\nexpected=8 actual=3\n  at test_component.py:121\n120 passed, 1 failed\n"
        raw_code = "".join(f"int component_{i}(int x) {{ return x + {i}; }}\n" for i in range(50))
        table = json.dumps([{"id": i, "state": "ok", "value": i * 3} for i in range(35)], indent=4)
        controls = [("repeated-worker", repeated, "log"), ("repeated-monitor", status, "log"),
                    ("unique-build-control", unique, "log"), ("test-failure-control", tests, "test"),
                    ("exact-code-control", raw_code, "code"), ("json-values-control", table, "json")]
        for name, text, _ in controls:
            (root / f"{name}.txt").write_bytes(text.encode())
        for path in root.rglob("*"):
            if path.is_file():
                results["fixtures_sha256"][path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()

        def run(build, argv, stdin=None):
            env = dict(os.environ, LESSTOKEN_DATA=str(root / f"state-{build}"))
            result = subprocess.run([executables[build], *argv], input=stdin, encoding="utf-8",
                                    capture_output=True, env=env, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stderr or result.stdout)
            return result.stdout

        schemas = {}
        for build in executables:
            listing = json.loads(run(build, ["mcp", "--root", str(root)], compact({
                "jsonrpc": "2.0", "id": 1, "method": "tools/list"})))
            schemas[build] = compact(listing["result"]["tools"])
        schemas["raw_reference"] = schemas["baseline"]

        def tool(build, name, arguments, payloads):
            request = compact({"tool": name, "arguments": arguments})
            output = run(build, ["ops", "--root", str(root)], request)
            payloads.extend([request, output])
            return output

        def record(name, payloads, calls):
            results["cases"].append({"name": name, "calls": calls, "tokens": {
                encoding: {build: sum(len(encoder.encode(p, disallowed_special=())) for p in parts)
                           for build, parts in payloads.items()} for encoding, encoder in encodings.items()}})

        # Baseline reads the exact required range, not a full file or inflated
        # default 100-line slice. Candidate must provide identical source evidence.
        source_cases = [("src/core.cpp", "json Store::stats()", 0, 14),
                        ("src/core.cpp", "Reduction reduce(", 2, 24),
                        ("src/query.cpp", "json project_query(", 0, 18),
                        ("src/main.cpp", "int main_impl(", 0, 18)]
        for relative, query, before, after in source_cases:
            rows = (root / relative).read_text(encoding="utf-8").splitlines()
            hits = [i for i, row in enumerate(rows, 1) if query.casefold() in row.casefold()]
            assert len(hits) == 1, (relative, query, hits)
            start, end = max(1, hits[0] - before), min(len(rows), hits[0] + after)
            expected = "\n".join(f"{i}: {rows[i-1]}" for i in range(start, end + 1))
            payloads = {build: [] for build in executables}
            old = json.loads(tool("baseline", "lt_search", {"path": relative, "query": query}, payloads["baseline"]))
            assert old["complete"] and len(old["matches"]) == 1
            read = tool("baseline", "lt_read", {"path": relative, "start": start, "count": end - start + 1}, payloads["baseline"])
            assert read.rstrip("\n") == expected
            new = json.loads(tool("candidate", "lt_search", {"path": relative, "query": query,
                                 "before": before, "after": after}, payloads["candidate"]))
            assert new["complete"] and len(new["matches"]) == 1
            assert not new["matches"][0].get("clipped")
            assert new["matches"][0]["context"] == expected
            payloads["raw_reference"] = payloads["baseline"][:]
            record(f"source:{relative}:{query}", payloads, {"baseline": 2, "candidate": 1, "raw_reference": 2})

        for name, raw, kind in controls:
            payloads = {build: [] for build in executables}
            outputs = {}
            for build in executables:
                outputs[build] = tool(build, "lt_read", {"path": f"{name}.txt", "kind": kind}, payloads[build])
                for ref in re.findall(r"ref=([0-9a-f]{32})", outputs[build]):
                    assert (root / f"state-{build}/results/{ref}.txt").read_bytes() == raw.encode()
            if name == "repeated-worker":
                assert "ERROR unique checkpoint mismatch expected=8 actual=3" in outputs["candidate"]
                assert "[previous 3 lines repeated 39 more times]" in outputs["candidate"]
            if name == "repeated-monitor":
                assert "[previous 3 lines repeated 24 more times]" in outputs["candidate"]
            if kind == "test":
                for value in ("FAIL test_boundary", "expected=8 actual=3", "test_component.py:121"):
                    assert value in outputs["baseline"] and value in outputs["candidate"]
            if kind == "json":
                assert outputs["candidate"].split("\n[LessToken")[0] == outputs["baseline"].split("\n[LessToken")[0]
                assert json.loads(outputs["candidate"].split("\n[LessToken")[0]) == json.loads(raw)
            if name in ("unique-build-control", "exact-code-control"):
                assert outputs["baseline"] == outputs["candidate"]
            payloads["raw_reference"] = payloads["baseline"][:]
            if kind in ("log", "test", "json"):
                payloads["raw_reference"][-1] = raw
            record(name, payloads, {"baseline": 1, "candidate": 1, "raw_reference": 1})

        results["totals"] = {}
        for encoding, encoder in encodings.items():
            totals = {build: sum(case["tokens"][encoding][build] for case in results["cases"])
                      for build in schemas}
            definitions = {build: len(encoder.encode(schema, disallowed_special=())) for build, schema in schemas.items()}
            total_with_definitions = {build: totals[build] + definitions[build] for build in schemas}
            results["totals"][encoding] = {"payload_tokens": totals, "tool_definition_tokens_once": definitions,
                "with_definitions_once": total_with_definitions,
                "reduction_vs_previous_plugin_percent": round(100 * (1 - total_with_definitions["candidate"] / total_with_definitions["baseline"]), 2),
                "reduction_vs_raw_reference_percent": round(100 * (1 - total_with_definitions["candidate"] / total_with_definitions["raw_reference"]), 2)}
        results["calls"] = {build: sum(case["calls"][build] for case in results["cases"]) for build in schemas}
        results["quality"] = "All exact source-window, unchanged-control, diagnostic and original-recovery assertions passed. Not an end-to-end model quality evaluation."
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"totals": results["totals"], "calls": results["calls"], "quality": results["quality"]}, indent=2))


if __name__ == "__main__":
    main()
