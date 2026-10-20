"""Exercise rewrite boundaries without executing project commands."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())


class HookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lesstoken-hooks-")
        self.root = Path(self.temp.name)
        (self.root / "job.log").write_text("INFO complete\n" * 100, encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def hook(self, command, mode="bypassPermissions", **fields):
        event = {"hook_event_name": "PreToolUse", "tool_name": "exec_command",
                 "permission_mode": mode, "cwd": str(self.root),
                 "tool_input": {"cmd": command, "shell": "powershell.exe", "login": False, **fields}}
        run = subprocess.run([EXE, "hook", "--rewrite-readonly"], input=json.dumps(event),
                             capture_output=True, encoding="utf-8", timeout=15,
                             env=dict(os.environ, LESSTOKEN_DATA=str(self.root / "state")))
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout) if run.stdout else None

    def test_powershell_read_preserves_execution_fields(self):
        result = self.hook("Get-Content -LiteralPath job.log", mode="default", yield_time_ms=1000)
        updated = result["hookSpecificOutput"]["updatedInput"]
        self.assertIn("reduce --kind log --file", updated["cmd"])
        self.assertEqual(updated["shell"], "powershell.exe")
        self.assertFalse(updated["login"])
        self.assertEqual(updated["yield_time_ms"], 1000)

    def test_foreign_shells_pass_through_all_rewrite_paths(self):
        for shell in ("cmd.exe", "bash.exe", "powershell-custom.exe"):
            for command in ("Get-Content job.log", "cmake --build build", "git status"):
                with self.subTest(shell=shell, command=command):
                    self.assertIsNone(self.hook(command, shell=shell))

    def test_build_wrap_requires_unrestricted_mode(self):
        for mode in ("default", "acceptEdits", "plan", "dontAsk", "unknown"):
            self.assertIsNone(self.hook("cmake --build build", mode=mode))
        self.assertIn("run --kind log", self.hook("cmake --build build")["hookSpecificOutput"]["updatedInput"]["cmd"])

    def test_empty_arguments_and_shell_expressions_are_unchanged(self):
        for command in ('python -m pytest -k ""', "python -m pytest -k ''",
                        "cmake --build build; echo done", "Get-Content job.log | Select-Object -First 1"):
            self.assertIsNone(self.hook(command))

    def test_outside_workdir_is_not_promoted_to_project_read(self):
        with tempfile.TemporaryDirectory(prefix="lesstoken-outside-") as other:
            (Path(other) / "job.log").write_text("private\n", encoding="utf-8")
            self.assertIsNone(self.hook("Get-Content job.log", workdir=other))

    def test_permission_fields_and_nested_calls_are_not_rewritten(self):
        for field in ("sandbox_permissions", "prefix_rule", "env"):
            self.assertIsNone(self.hook("git status", **{field: "test"}))
        self.assertIsNone(self.hook('text(await tools.exec_command({cmd:"git status"}))'))


if __name__ == "__main__":
    unittest.main()
