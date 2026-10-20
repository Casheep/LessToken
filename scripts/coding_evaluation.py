"""C++ repair cases with functional graders, not exact-patch matching.

Preparation and grader checks make no model calls. Submitted functions are
compiled/run only as part of an explicitly requested coding evaluation.
"""
import json
import os
from pathlib import Path
import subprocess

CASES = [
    {"id": "safe-pagination", "file": "pagination.hpp",
     "prompt": "Fix page_values in pagination.hpp. Preserve the signature. Return an empty vector when offset is at/past the end or limit is zero; otherwise return at most limit values starting at offset. Handle size_t overflow. Return JSON {code: ...} containing the complete corrected function only. Do not modify tests.",
     "broken": "std::vector<int> page_values(const std::vector<int>& rows, std::size_t offset, std::size_t limit) {\n  auto end = std::min(rows.size(), offset + limit);\n  return {rows.begin() + offset, rows.begin() + end};\n}\n",
     "reference": "std::vector<int> page_values(const std::vector<int>& rows, std::size_t offset, std::size_t limit) {\n  if(offset >= rows.size()) return {};\n  auto count = std::min(limit, rows.size()-offset);\n  return {rows.begin()+offset, rows.begin()+offset+count};\n}\n",
     "tests": """int main() {
  const std::vector<int> values{2,4,6,8};
  const auto huge=std::numeric_limits<std::size_t>::max();
  if(page_values(values,0,2)!=std::vector<int>({2,4})) return 1;
  if(page_values(values,1,huge)!=std::vector<int>({4,6,8})) return 2;
  if(!page_values(values,4,3).empty()) return 3;
  if(!page_values(values,huge,2).empty()) return 4;
  if(!page_values(values,1,0).empty()) return 5;
  if(!page_values({},0,huge).empty()) return 6;
  for(std::size_t offset=0;offset<8;++offset) for(std::size_t limit=0;limit<8;++limit) {
    std::vector<int> expected;
    for(std::size_t i=offset;i<values.size() && expected.size()<limit;++i) expected.push_back(values[i]);
    if(page_values(values,offset,limit)!=expected) return 7;
  }
  return 0;
}
"""},
    {"id": "overlapping-ranges", "file": "ranges.hpp",
     "prompt": "Fix merge_windows in ranges.hpp. Inclusive valid intervals may be unsorted, duplicated or nested. Return sorted intervals merging overlaps only; adjacent nonoverlapping intervals remain separate. Preserve the signature and all covered values, including negative endpoints. Return JSON {code: ...} containing the complete corrected function only. Do not modify tests.",
     "broken": "std::vector<std::pair<int,int>> merge_windows(std::vector<std::pair<int,int>> ranges) {\n  std::vector<std::pair<int,int>> out;\n  for(auto range:ranges) {\n    if(!out.empty() && range.first <= out.back().second) out.back().second=range.second;\n    else out.push_back(range);\n  }\n  return out;\n}\n",
     "reference": "std::vector<std::pair<int,int>> merge_windows(std::vector<std::pair<int,int>> ranges) {\n  std::sort(ranges.begin(),ranges.end());\n  std::vector<std::pair<int,int>> out;\n  for(auto range:ranges) {\n    if(!out.empty() && range.first <= out.back().second) out.back().second=std::max(out.back().second,range.second);\n    else out.push_back(range);\n  }\n  return out;\n}\n",
     "tests": """int main() {
  using R=std::vector<std::pair<int,int>>;
  if(merge_windows({{10,20},{1,4},{3,7},{12,15},{1,4}})!=R({{1,7},{10,20}})) return 1;
  if(merge_windows({{1,2},{3,4}})!=R({{1,2},{3,4}})) return 2;
  if(merge_windows({{-4,-2},{-3,1},{9,9}})!=R({{-4,1},{9,9}})) return 3;
  if(!merge_windows({}).empty()) return 4;
  R ranges{{1,10},{3,4},{10,12},{16,19},{18,20}};
  do { if(merge_windows(ranges)!=R({{1,12},{16,20}})) return 5; }
  while(std::next_permutation(ranges.begin(),ranges.end()));
  return 0;
}
"""},
]
HEADERS = "#include <vector>\n#include <algorithm>\n#include <utility>\n#include <cstddef>\n#include <limits>\n"


def prepare(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=False)
    public = []
    for case in CASES:
        folder = root / case["id"]
        folder.mkdir()
        (folder / case["file"]).write_text(HEADERS + case["broken"], encoding="utf-8")
        (folder / "tests.cpp").write_text(HEADERS + f'#include "{case["file"]}"\n' + case["tests"], encoding="utf-8")
        (folder / "prompt.txt").write_text(case["prompt"], encoding="utf-8")
        public.append({"id": case["id"], "file": case["file"], "prompt": case["prompt"]})
    (root / "plan.json").write_text(json.dumps(public, indent=2), encoding="utf-8")


def grade(case_id, answer, compiler, folder):
    case = next(case for case in CASES if case["id"] == case_id)
    if not isinstance(answer, dict) or set(answer) != {"code"} or not isinstance(answer["code"], str) or len(answer["code"]) > 16000:
        return {"passed": False, "phase": "answer-format"}
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=False)
    source, binary = folder / "submission.cpp", folder / "submission.exe"
    source.write_text(HEADERS + answer["code"] + "\n" + case["tests"], encoding="utf-8")
    compiler = Path(compiler).resolve()
    env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ.get("PATH", ""))
    command = [str(compiler), "-std=c++20", "-O1", "-Wall", "-Wextra", str(source), "-o", str(binary)]
    if os.name == "nt":
        command.extend(["-static", "-static-libgcc", "-static-libstdc++"])
    compiled = subprocess.run(command, capture_output=True, timeout=30, env=env)
    (folder / "compile.stderr.txt").write_bytes(compiled.stderr)
    if compiled.returncode:
        return {"passed": False, "phase": "compile"}
    try:
        run = subprocess.run([str(binary)], capture_output=True, timeout=5)
    except subprocess.TimeoutExpired:
        return {"passed": False, "phase": "timeout"}
    return {"passed": run.returncode == 0, "phase": "functional-tests", "exit_code": run.returncode}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", required=True, help="New local fixture folder; no model calls")
    prepare(parser.parse_args().prepare)
    print("Prepared two C++ repair tasks. No model sessions started.")
