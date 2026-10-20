"""Prepare paired task evaluations; --run explicitly starts model sessions.

Default: prepare reviewable prompts, fixtures and expected answers only.
Runs use saved Codex login, the same explicit model/effort in both arms, fresh
sessions with explicitly selected permissions and project-confined MCP tools.
Tasks request read-only behavior and verify fixture hashes. No key is read or exported.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def prepare(root, source, suite="pilot",native_tools=False):
    policy=" Use available local tools for project files. Do not use web, external services or other agents. Do not modify workspace files. Return only the requested JSON." if native_tools else " Use only the provided project MCP tools; do not use shell, web, external services or other agents. Read originals when evidence is incomplete. Return only the requested JSON."
    if native_tools:
        policy+=' For exec_command use shell="powershell.exe", login=false, and omit sandbox_permissions and prefix_rule; use literal file paths.'
    if suite=="coding":
        import coding_evaluation
        coding_evaluation.prepare(root/"fixtures")
        cases=[{"id":case["id"],"prompt":case["prompt"]+policy,
                "expected":None,"grader":"cpp-functional"} for case in coding_evaluation.CASES]
        manifest={"source_commit":source,"cases":cases,"suite":suite,"fixture_sha256":{
            path.relative_to(root/"fixtures").as_posix():hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (root/"fixtures").rglob("*") if path.is_file()},
            "scope":"C++ repair pilot. Answers are compiled and functionally tested; not exact-patch matched."}
        return manifest
    fixtures = root / "fixtures"
    fixtures.mkdir(parents=True)
    block = "INFO queue scan finished\nINFO input records indexed\nINFO checkpoint stored\n"
    log = "BEGIN local job\n" + block * 30
    log += "ERROR E17 record=42 expected=8 actual=3\n  at worker.cpp:87\n"
    log += block * 20 + "ERROR E29 record=19 expected=1 actual=0\n  at writer.cpp:54\nEND local job\n"
    (fixtures / "worker.log").write_text(log, encoding="utf-8")
    diagnostic = ("WARNING W7 transient retry\nINFO retry context retained\n") * 9
    diagnostic += "Traceback: unique decoder failure\n  at decoder.py:91\nValueError: invalid continuation byte\n"
    (fixtures / "diagnostics.log").write_text(diagnostic, encoding="utf-8")
    (fixtures / "core.cpp").write_bytes(subprocess.check_output(["git", "show", f"{source}:src/core.cpp"]))
    rows = [{"id": i, "status": "failed" if i % 13 == 0 else "passed", "attempts": i % 4 + 1,
             "component": f"component-{i % 7}", "detail": "fixture record only"} for i in range(1, 101)]
    (fixtures / "rows.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    cases = [
        {"id": "log-diagnosis", "prompt": "Inspect worker.log. Return the unique errors in encounter order as JSON {errors:[{code,record,expected,actual,file,line}]}. Include every error, with integer record/expected/actual/line values.",
         "expected": {"errors": [{"code": "E17", "record": 42, "expected": 8, "actual": 3, "file": "worker.cpp", "line": 87},
                                 {"code": "E29", "record": 19, "expected": 1, "actual": 0, "file": "writer.cpp", "line": 54}]}},
        {"id": "diagnostic-retention", "prompt": "Inspect diagnostics.log. Count occurrences of warning W7 and identify the final exception and its source location. Return JSON {warning_count,exception,file,line}, with integer warning_count and line. Exception is the exception class, not its message.",
         "expected": {"warning_count": 9, "exception": "ValueError", "file": "decoder.py", "line": 91}},
        {"id": "source-understanding", "prompt": "Inspect core.cpp, specifically Store::stats and Store::record. Does net_bytes_removed subtract shown from input, include retrieval records with input_bytes=0, and measure provider billing? Return JSON {subtracts_shown_from_input,includes_zero_input_records,measures_billing}, all booleans. Inspect implementation, not just comments.",
         "expected": {"subtracts_shown_from_input": True, "includes_zero_input_records": True, "measures_billing": False}},
        {"id": "data-query", "prompt": "Inspect rows.json. Return JSON {ids:[...]} containing IDs of records whose status is failed, in their original order. IDs must be integers.",
         "expected": {"ids": [row["id"] for row in rows if row["status"] == "failed"]}},
    ]
    if suite in ("holdout","validation"):
        original=cases
        cases=[dict(case) for case in cases]
        wording=[
            "请从 worker.log 找出所有不同错误，按出现顺序返回 JSON {errors:[{code,record,expected,actual,file,line}]}。record、expected、actual、line 为整数。",
            "In diagnostics.log, count W7 warnings and identify the final exception class and location. Return JSON {warning_count,exception,file,line}; counts and line are integers.",
            "For core.cpp, determine from Store::stats and Store::record whether net_bytes_removed is input minus shown, zero-input retrieval records are included, and provider billing is measured. Return JSON {subtracts_shown_from_input,includes_zero_input_records,measures_billing}, all booleans.",
            "From rows.json select IDs whose status is failed in original order. Return JSON {ids:[...]} with integer IDs."]
        for case,prompt in zip(cases,wording):
            case["id"]="fallback-"+case["id"]
            case["prompt"]=prompt
        big_source="".join(f"int helper_{i:04}(int value) {{ return value + {i}; }} // independent local unit\n" for i in range(1500))
        big_source+="int final_value() { return 731; }\n"
        (fixtures/"big-code.cpp").write_text(big_source,encoding="utf-8")
        big_rows=[{"id":i,"status":"failed" if i%97==0 else "passed","detail":"local fixture record "*5} for i in range(1,1001)]
        (fixtures/"big-rows.json").write_text(json.dumps(big_rows,indent=2),encoding="utf-8")
        cases.extend([
            {"id":"oversize-source","prompt":"Inspect big-code.cpp. Locate final_value and return its name and integer return constant as JSON {function,return_value}.","expected":{"function":"final_value","return_value":731}},
            {"id":"oversize-json","prompt":"Inspect big-rows.json. Return JSON {ids:[...]} with integer IDs of failed records, preserving source order.","expected":{"ids":[i for i in range(97,1001,97)]}},
        ])
        if suite=="validation":
            cases=original+cases
            import coding_evaluation
            coding_evaluation.prepare(fixtures/"coding")
            cases.extend({"id":case["id"],"prompt":case["prompt"],"expected":None,"grader":"cpp-functional", "fixture_subdir":"coding/"+case["id"]} for case in coding_evaluation.CASES)
    for case in cases:
        case["prompt"] += policy
    manifest = {"source_commit": source, "cases": cases,
                "fixture_sha256": {path.relative_to(fixtures).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in fixtures.rglob("*") if path.is_file()},
                "scope": "Declared task suite; passing is not proof for all coding work. Baseline is the previous plugin, not plugin-off. No result is accepted without terminal usage and quality validation."}
    (root / "plan.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def parse_events(text, expected):
    completed, failures, messages = [], [], []
    for row in text.splitlines():
        try:
            event = json.loads(row)
        except ValueError:
            failures.append("malformed_event")
            continue
        if not isinstance(event, dict):
            failures.append("malformed_event")
            continue
        if event.get("type") == "turn.completed":
            completed.append(event.get("usage"))
        if event.get("type") in ("turn.failed", "error"):
            failures.append(event["type"])
        if event.get("type") == "item.completed" and event.get("item", {}).get("type") == "agent_message":
            messages.append(event["item"].get("text", ""))
    usage = {key: 0 for key in ("input_tokens", "cached_input_tokens", "output_tokens", "reasoning_output_tokens")}
    for counts in completed:
        if not isinstance(counts, dict) or not all(key in counts for key in usage):
            return {"valid": False, "reason": "Incomplete usage"}
        for key in usage:
            value = counts.get(key, 0)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                return {"valid": False, "reason": "Invalid usage"}
            usage[key] += value
        if counts["cached_input_tokens"] > counts["input_tokens"] or counts["reasoning_output_tokens"] > counts["output_tokens"]:
            return {"valid": False, "reason": "Inconsistent per-turn usage"}
    if usage["cached_input_tokens"] > usage["input_tokens"] or usage["reasoning_output_tokens"] > usage["output_tokens"]:
        return {"valid": False, "reason": "Inconsistent usage"}
    usage["total_tokens"] = usage["input_tokens"] + usage["output_tokens"]
    if failures or not completed or not messages:
        result = {"valid": False, "reason": "Missing successful terminal usage or final answer"}
        if completed:
            result["observed_usage"] = usage
        return result
    try:
        answer = json.loads(messages[-1])
    except ValueError:
        return {"valid": False, "reason": "Final answer is not exact JSON", "usage": usage}
    # Strict JSON encoding also distinguishes true from 1, unlike Python equality.
    if expected is None:
        return {"valid":True,"answer":answer,"usage":usage}
    passed = json.dumps(answer, sort_keys=True) == json.dumps(expected, sort_keys=True)
    return {"valid": True, "passed": passed, "usage": usage}


def compare_pairs(results, cases):
    """Require the entire predeclared cohort; never cherry-pick passing cases."""
    ids = [case["id"] for case in cases]
    if len(set(ids)) != len(ids):
        raise ValueError("Duplicate case IDs")
    pairs = {}
    for result in results:
        key = (result.get("case"), result.get("arm"))
        if key[0] not in ids or key[1] not in ("baseline", "candidate") or key in pairs:
            raise ValueError("Unexpected or duplicate result")
        pairs[key] = result
    expected = {(case, arm) for case in ids for arm in ("baseline", "candidate")}
    if set(pairs) != expected or any(not item.get("valid") or "usage" not in item for item in pairs.values()):
        return {"conclusive": False, "goal_verified": False, "reason": "Missing, invalid or incomplete task results"}
    totals = {arm: sum(pairs[case, arm]["usage"]["total_tokens"] for case in ids) for arm in ("baseline", "candidate")}
    if not totals["baseline"]:
        return {"conclusive": False, "goal_verified": False, "reason": "Zero baseline usage"}
    regressions = [case for case in ids if pairs[case, "baseline"].get("passed") and not pairs[case, "candidate"].get("passed")]
    passed = {arm: sum(bool(pairs[case, arm].get("passed")) for case in ids) for arm in totals}
    reduction = 100 * (1 - totals["candidate"] / totals["baseline"])
    # This pilot can reject the goal, but cannot establish broad coding quality.
    return {"conclusive": True, "goal_verified": False, "total_tokens": totals,
            "reduction_percent": round(reduction, 2), "passed": passed, "regressions": regressions,
            "pilot_gate_passed": reduction >= 40 and passed["candidate"] == len(ids) and not regressions,
            "scope": "Declared pilot tasks only. Baseline is previous plugin. Broader quality, plugin-off usage and installed-host validation remain required."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, help="New local output folder; never overwrites")
    parser.add_argument("--source-ref", default="a57b5fd")
    parser.add_argument("--model", required=True)
    parser.add_argument("--effort", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--codex", help="Codex CLI path, required only with --run")
    parser.add_argument("--prompt-hooks",action="store_true",help="Test native pre-model hooks in both arms, with vetted isolated hook sources")
    parser.add_argument("--suite",choices=("pilot","coding","holdout","validation"),default="pilot")
    parser.add_argument("--compiler",help="C++ compiler path, required for coding runs")
    parser.add_argument("--native-tools",action="store_true",help="Allow normal native read-only tools in both arms")
    parser.add_argument("--baseline-mode",choices=("previous-plugin","builtin"),default="previous-plugin",help="builtin disables MCP in baseline and enables native tools in both arms")
    parser.add_argument("--sandbox-mode",choices=("read-only","workspace-write","danger-full-access"),help="Explicit test process profile; full access is only for reviewed generated fixtures; fixture integrity is audited")
    parser.add_argument("--case",action="append",help="Run only explicitly named diagnostic cases; excluded from full-suite claims")
    parser.add_argument("--run", action="store_true", help="Start eight model sessions using saved login; requires user authorization")
    args = parser.parse_args()
    if args.baseline_mode=="builtin":
        args.native_tools=True
    if args.sandbox_mode is None:
        args.sandbox_mode="workspace-write" if args.native_tools else "read-only"
    if args.run and not args.codex:
        parser.error("--run requires --codex")
    if args.run and args.suite in ("coding","validation") and not args.compiler:
        parser.error("coding runs require --compiler")
    if args.run and args.prompt_hooks:
        # The automation trust flag must not implicitly approve unrelated local
        # handlers. User config is ignored; refuse other hooks.json/project hooks.
        import tomllib
        home=Path(os.environ.get("CODEX_HOME",Path.home()/".codex"))
        if (home/"hooks.json").exists():
            parser.error("Isolated hook evaluation refuses unrelated user hooks.json")
        for ancestor in (Path(args.directory).resolve(),*Path(args.directory).resolve().parents):
            config=ancestor/".codex/config.toml"
            if (ancestor/".codex/hooks.json").exists() or (config.exists() and tomllib.loads(config.read_text(encoding="utf-8")).get("hooks")):
                parser.error("Isolated hook evaluation refuses unrelated project hooks")
    source = subprocess.check_output(["git", "rev-parse", "--verify", args.source_ref + "^{commit}"], text=True).strip()
    root = Path(args.directory).resolve()
    root.mkdir(parents=True, exist_ok=False)
    plan = prepare(root, source,args.suite,args.native_tools)
    if args.case:
        selected=set(args.case)
        if selected-{case["id"] for case in plan["cases"]}:
            parser.error("Unknown diagnostic case")
        plan["cases"]=[case for case in plan["cases"] if case["id"] in selected]
        plan["diagnostic_subset"]=True
    plan.update(model=args.model, effort=args.effort,suite=args.suite,prompt_hooks=args.prompt_hooks,native_tools=args.native_tools,baseline_mode=args.baseline_mode,sandbox_mode=args.sandbox_mode)
    if args.baseline_mode=="builtin":
        plan["scope"]="Declared task suite; plugin-off native tools vs candidate. No result accepted without terminal usage and quality validation; not proof for all coding work."
    binaries = {}
    for arm,path in (("baseline",args.baseline),("candidate",args.candidate)):
        binary=Path(path).resolve()
        binaries[arm]=str(binary)
        if args.run:
            frozen=root/(arm+".exe")
            shutil.copyfile(binary,frozen)
            binaries[arm]=str(frozen)
    plan["binary_sha256"]={arm:hashlib.sha256(Path(path).read_bytes()).hexdigest() for arm,path in binaries.items()}
    hook_commands={}
    if args.prompt_hooks:
        script=Path(__file__).resolve().parents[1]/"plugins/lesstoken/hooks/invoke.ps1"
        plan["hook_script_sha256"]=hashlib.sha256(script.read_bytes()).hexdigest()
        for arm,path in binaries.items():
            staged=root/(arm+"-hook")
            (staged/"hooks").mkdir(parents=True)
            (staged/"bin").mkdir()
            shutil.copyfile(path,staged/"bin/lesstoken.exe")
            target=staged/"hooks/invoke.ps1"
            shutil.copyfile(script,target)
            hook_commands[arm]='powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+target.as_posix()+'" prompt'
    (root / "plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
    if not args.run:
        print(f"Prepared {len(plan['cases'])} task pairs; no model sessions started. Review plan.json and fixtures.")
        return
    results = []
    for index, case in enumerate(plan["cases"]):
        # Alternate arm order to limit systematic cache/time ordering bias.
        arms = list(binaries.items())
        if index % 2:
            arms.reverse()
        for arm, binary in arms:
            work = root / arm / case["id"]
            fixture=root/"fixtures"/case.get("fixture_subdir",case["id"]) if args.suite=="coding" or "fixture_subdir" in case else root/"fixtures"
            shutil.copytree(fixture, work)
            def fingerprint():
                return {path.relative_to(work).as_posix():hashlib.sha256(path.read_bytes()).hexdigest() for path in work.rglob("*") if path.is_file()}
            before_files=fingerprint()
            command = [str(Path(args.codex).resolve()), "exec", "--json", "--ephemeral", "--ignore-user-config",
                       "--skip-git-repo-check", "--sandbox", args.sandbox_mode, "--cd", str(work), "--model", args.model]
            settings = {"model_reasoning_effort": json.dumps(args.effort), "web_search": '"disabled"',"approval_policy":'"never"',
                        "mcp_servers.lt_eval.command": json.dumps(str(Path(binary).resolve()).replace("\\", "/")),
                        "mcp_servers.lt_eval.args": '["mcp"]', "mcp_servers.lt_eval.required": "true"}
            settings['projects.'+json.dumps(work.as_posix())+'.trust_level']='"trusted"'
            for feature in ("plugins", "apps", "memories", "hooks", "multi_agent", "shell_tool", "unified_exec"):
                settings[f"features.{feature}"] = "false"
            if args.native_tools:
                settings["features.shell_tool"]="true"
                settings["features.unified_exec"]="true"
            if args.baseline_mode=="builtin" and arm=="baseline":
                for key in list(settings):
                    if key.startswith("mcp_servers."):
                        del settings[key]
            settings["features.skip_host_skill_discovery"] = "true"
            if args.prompt_hooks:
                settings["features.hooks"]="true"
                settings["hooks.UserPromptSubmit"]='[{hooks=[{type="command",command='+json.dumps(hook_commands[arm])+',timeout=10,additionalContextLimit=16000}]}]'
                # Only the reviewed, frozen local handler above is enabled. No
                # persisted trust records or installed user hooks are modified.
                command.append("--dangerously-bypass-hook-trust")
            for key, value in settings.items():
                command.extend(["-c", f"{key}={value}"])
            # No shell interpolation; logs remain local in the explicit output folder.
            state = work.parent / (case["id"] + "-state")
            env = dict(os.environ, LESSTOKEN_DATA=str(state))
            # A standalone evaluation has its own thread/tool lifecycle. Never
            # route its shell requests through the parent's already-used app IPC
            # call or thread IDs. Authentication and sandbox settings are kept.
            for key in ("CODEX_APP_TOOLS_PIPE_PATH","CODEX_TOOL_CALL_ID","CODEX_THREAD_ID","CODEX_SESSION_ID","CODEX_INTERNAL_ORIGINATOR_OVERRIDE","CODEX_PERMISSION_PROFILE","CODEX_CI","CODEX_SHELL"):
                env.pop(key,None)
            completed = subprocess.run(command + ["-"], input=case["prompt"], encoding="utf-8",
                                       capture_output=True, env=env, timeout=600)
            (work.parent / (case["id"] + ".jsonl")).write_text(completed.stdout, encoding="utf-8")
            (work.parent / (case["id"] + ".stderr.txt")).write_text(completed.stderr, encoding="utf-8")
            result = parse_events(completed.stdout, case["expected"])
            if args.native_tools and "exec_command failed" in completed.stderr and "blocked by policy" in completed.stderr:
                result.update(valid=False,reason="Native tool baseline unavailable under execution policy")
            result["fixture_unchanged"]=fingerprint()==before_files
            if not result["fixture_unchanged"]:
                result.update(valid=False,reason="Workspace fixture changed despite read-only task")
            if completed.returncode:
                result["valid"] = False
                result["reason"] = "CLI failed"
            if case.get("grader")=="cpp-functional" and result.get("valid"):
                import coding_evaluation
                verdict=coding_evaluation.grade(case["id"],result.pop("answer"),args.compiler,root/"grades"/arm/case["id"])
                result.update(passed=verdict["passed"],functional_grade=verdict)
            result["prefetch_events"]=sum(1 for path in (state/"events").glob("*.json") if json.loads(path.read_text()).get("kind")=="prompt-prefetch")
            result.update(case=case["id"], arm=arm)
            results.append(result)
            (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(json.dumps(result), flush=True)
            if result.get("reason")=="Native tool baseline unavailable under execution policy":
                summary={"conclusive":False,"goal_verified":False,"reason":"Stopped: native command policy prevents a valid paired baseline", "baseline_mode":args.baseline_mode}
                (root/"summary.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
                return
    summary = compare_pairs(results, plan["cases"])
    summary["baseline_mode"]=args.baseline_mode
    if args.baseline_mode=="builtin":
        summary["scope"]="Declared suite; plugin-off native tools vs candidate with native tools, MCP and prompt snapshots. Same model/effort and sandbox; task fixtures audited unchanged. Not a subscription quota or universal quality guarantee."
    (root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
