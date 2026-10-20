"""Check that a parser-only safety fix preserves frozen model input snapshots.

No model calls. Random original_ref IDs are the only normalized field.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-directory",required=True)
    parser.add_argument("--candidate",required=True)
    parser.add_argument("--output",required=True)
    args=parser.parse_args()
    root=Path(args.run_directory).resolve()
    candidate=Path(args.candidate).resolve()
    frozen=root/"candidate.exe"
    plan=json.loads((root/"plan.json").read_text(encoding="utf-8"))
    cases=[]
    with tempfile.TemporaryDirectory(prefix="lesstoken-input-check-") as temp:
        for case in plan["cases"]:
            work=root/"fixtures"/case["fixture_subdir"] if "fixture_subdir" in case else root/"fixtures"
            normalized=[]
            for index,binary in enumerate((frozen,candidate)):
                event={"hook_event_name":"UserPromptSubmit","cwd":str(work),"prompt":case["prompt"]}
                run=subprocess.run([str(binary),"hook","--prompt"],input=json.dumps(event),encoding="utf-8",capture_output=True,
                                   env=dict(os.environ,LESSTOKEN_DATA=str(Path(temp)/str(index))),timeout=15,check=True)
                if not run.stdout:
                    normalized.append(None)
                    continue
                value=json.loads(run.stdout)
                header,data=value["hookSpecificOutput"]["additionalContext"].split("\n",1)
                snapshot=json.loads(data)
                snapshot.pop("original_ref")
                normalized.append({"header":header,"snapshot":snapshot})
            cases.append({"case":case["id"],"same":normalized[0]==normalized[1],"prefetched":normalized[1] is not None})
    result={"scope":"Only prompt snapshot behavior; normalizes random original_ref IDs only. No model calls.",
            "frozen_binary_sha256":hashlib.sha256(frozen.read_bytes()).hexdigest(),
            "current_binary_sha256":hashlib.sha256(candidate.read_bytes()).hexdigest(),
            "all_equal":all(case["same"] for case in cases),"cases":cases}
    Path(args.output).write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result,indent=2))
    if not result["all_equal"]:
        raise SystemExit(1)


if __name__=="__main__":
    main()
