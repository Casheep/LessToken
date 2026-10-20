"""Check pre-model snapshot scope, fidelity, budgets and safe fallbacks."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())


class PrefetchTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="lesstoken-prefetch-")
        self.root=Path(self.temp.name)
        self.env=dict(os.environ,LESSTOKEN_DATA=str(self.root/"state"))

    def tearDown(self):
        self.temp.cleanup()

    def write(self,name,text):
        path=self.root/name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(text.encode())
        return path

    def call(self,prompt,event="UserPromptSubmit"):
        run=subprocess.run([EXE,"hook","--prompt"],input=json.dumps({"hook_event_name":event,
            "cwd":str(self.root),"prompt":prompt}),encoding="utf-8",capture_output=True,env=self.env,timeout=15)
        self.assertEqual(run.returncode,0,run.stderr)
        if not run.stdout:
            return None
        context=json.loads(run.stdout)["hookSpecificOutput"]["additionalContext"]
        self.assertLessEqual(len(context.encode()),16000)
        return json.loads(context.split("\n",1)[1])

    def test_exact_source_complete_and_recoverable(self):
        raw='// 中文\nint count(int x) { return x+1; }\n'
        self.write("code.cpp",raw)
        snap=self.call("Inspect code.cpp. Return the behavior.")
        self.assertTrue(snap["complete"] and snap["lossless"])
        self.assertEqual(snap["text"],raw)
        self.assertEqual((self.root/"state/results"/(snap["original_ref"]+".txt")).read_bytes(),raw.encode())

    def test_log_repeats_and_all_diagnostics(self):
        raw=("INFO stage one completed\nINFO stage two completed\n")*30+'ERROR unique failure\n  at task.cpp:12\n'
        self.write("job.log",raw)
        snap=self.call("Inspect job.log. Find errors.")
        self.assertIn("29 more times",snap["text"])
        self.assertIn("ERROR unique failure\n  at task.cpp:12",snap["text"])

    def test_json_keeps_duplicate_keys_and_exact_numbers(self):
        raw='[\n  {"n":1.000,"n":2,"text":"a b"}\n]'+" "*600
        self.write("rows.json",raw)
        snap=self.call("Read rows.json. Inspect values.")
        self.assertEqual(snap["text"],'[{"n":1.000,"n":2,"text":"a b"}]')

    def test_format_sensitive_requests_keep_original(self):
        for name,raw in (("rows.json", '[\n  {"n": 1}\n]'+" "*600),
                         ("job.log", "INFO done\n"*80)):
            self.write(name,raw)
            for detail in ("Count the number of lines", "检查缩进和字节"):
                snap=self.call(f"Inspect {name}. {detail}")
                self.assertEqual(snap["text"],raw)
                self.assertEqual(snap["representation"],"passthrough")

    def test_legacy_post_hook_never_stops_agent(self):
        event={"hook_event_name":"PostToolUse","tool_name":"Bash",
               "tool_input":{"command":"cat job.log"},"tool_response":"INFO done\n"*1000}
        run=subprocess.run([EXE,"hook","--post"],input=json.dumps(event),encoding="utf-8",
                           capture_output=True,env=self.env,timeout=15)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertEqual(run.stdout,"")

    def test_negation_ambiguity_and_urls_do_nothing(self):
        self.write("code.cpp","int x=0;\n")
        for prompt in ("Don't inspect code.cpp", "Inspect code.cpp but do not read its contents",
                       "The file code.cpp may be relevant", "Explain what Read code.cpp means",
                       "Read the literal filename code.cpp aloud", "Inspect this example that mentions code.cpp",
                       "For code.cpp, rename it", "From code.cpp to code.py", "In code.cpp, do not read anything",
                       "From code.cpp count characters in the literal filename only",
                       "Inspect https://example.com/code.cpp", "Run code.cpp", "Delete code.cpp"):
            with self.subTest(prompt=prompt):
                self.assertIsNone(self.call(prompt))

    def test_polite_and_file_first_read_requests(self):
        self.write("code.cpp","int value=7;\n")
        for prompt in ("Please inspect code.cpp", "Can you read code.cpp", "Could you review code.cpp",
                       "请帮我查看 code.cpp", "你帮我看一下 code.cpp", "From code.cpp select identifiers",
                       "In code.cpp, count definitions", "For code.cpp, determine its behavior",
                       "请从 code.cpp 找出函数"):
            with self.subTest(prompt=prompt):
                self.assertEqual(self.call(prompt)["text"],"int value=7;\n")

    def test_sensitive_hidden_and_parent_paths_do_nothing(self):
        for name in ("auth.json","config.json","passwords.txt","api-keys.json",".private.txt",".secret/code.cpp"):
            self.write(name,"private fixture value")
            with self.subTest(name=name):
                self.assertIsNone(self.call("Inspect "+name))
        self.assertIsNone(self.call("Inspect ../outside.cpp"))

    def test_budget_binary_missing_and_unsupported_event(self):
        self.write("large.cpp","x"*66000)
        self.write("escape.cpp",'"'*15000)
        self.write("binary.txt","a\0b")
        for name in ("large.cpp","escape.cpp","binary.txt","missing.cpp"):
            self.assertIsNone(self.call("Inspect "+name))
        self.assertIsNone(self.call("Inspect missing.cpp",event="PreToolUse"))

    def test_chinese_quoted_path_freshness_and_opt_out(self):
        self.write("src/code.cpp","int old_value=1;\n")
        self.assertIn("old_value",self.call("查看 `src/code.cpp` 的实现")["text"])
        self.write("src/code.cpp","int new_value=2;\n")
        self.assertIn("new_value",self.call("Inspect src/code.cpp")["text"])
        self.env["LESSTOKEN_PREFETCH"]="0"
        self.assertIsNone(self.call("Inspect src/code.cpp"))

    def test_only_first_explicit_file_is_read(self):
        self.write("first.cpp","int first=1;\n")
        self.write("second.cpp","int second=2;\n")
        snap=self.call("Inspect first.cpp and second.cpp")
        self.assertEqual(snap["path"],"first.cpp")
        self.assertNotIn("second",snap["text"])

    def test_hardlink_alias_is_not_prefetched(self):
        target=self.write("auth.json",'{"private":"local test only"}')
        os.link(target,self.root/"code.cpp")
        self.assertIsNone(self.call("Inspect code.cpp"))

    def test_filename_prefixes_are_not_different_operands(self):
        self.write("code.cpp","int x=1;\n")
        for operand in ("code.cpp.backup",'"code.cpp.backup"',"code.cpp备用.txt",'"code.cpp, alternate.txt"',"code.cpp,other.txt"):
            with self.subTest(operand=operand):
                self.assertIsNone(self.call("Inspect "+operand))

    def test_symlink_alias_is_not_prefetched(self):
        target=self.write("auth.json",'{"private":"local test only"}')
        try:
            os.symlink(target,self.root/"alias.cpp")
        except OSError:
            self.skipTest("Symbolic links require host permission")
        self.assertIsNone(self.call("Inspect alias.cpp"))

    def test_packaged_wrapper_shares_explicit_mcp_data_directory(self):
        package=self.root/"package"
        (package/"hooks").mkdir(parents=True)
        (package/"bin").mkdir()
        shutil.copyfile(EXE,package/"bin/lesstoken.exe")
        wrapper=Path(__file__).resolve().parents[1]/"plugins/lesstoken/hooks/invoke.ps1"
        shutil.copyfile(wrapper,package/"hooks/invoke.ps1")
        explicit=self.root/"configured-state"
        (package/".mcp.json").write_text(json.dumps({"mcpServers":{"lesstoken":{"env":{"LESSTOKEN_DATA":str(explicit)}}}}),encoding="utf-8")
        self.write("code.cpp","int value=7;\n")
        event={"hook_event_name":"UserPromptSubmit","cwd":str(self.root),"prompt":"Inspect code.cpp"}
        run=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",str(package/"hooks/invoke.ps1"),"prompt"],
            input=json.dumps(event),encoding="utf-8",capture_output=True,cwd=self.root,env=self.env,timeout=20)
        self.assertEqual(run.returncode,0,run.stderr)
        data=json.loads(json.loads(run.stdout)["hookSpecificOutput"]["additionalContext"].split("\n",1)[1])
        self.assertEqual((explicit/"results"/(data["original_ref"]+".txt")).read_bytes(),b'int value=7;\n')
        self.assertFalse((self.root/"state").exists())


if __name__=="__main__":
    unittest.main()
