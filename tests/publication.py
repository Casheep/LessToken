"""Check public export boundaries using temporary synthetic packages."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("prepare_github", Path(__file__).resolve().parents[1] / "scripts/prepare_github.py")
public = importlib.util.module_from_spec(spec)
spec.loader.exec_module(public)


class PublicationTests(unittest.TestCase):
    def test_secret_and_personal_path_checks_hide_values(self):
        for text in ("sk-" + "a" * 30, "github_pat_" + "b" * 40,
                     "C:" + "/Users/Someone/private.txt"):
            with self.assertRaises(ValueError) as caught:
                public.scan("sample.txt", text.encode())
            self.assertNotIn(text, str(caught.exception))

    def test_traversal_and_absolute_members_rejected(self):
        with tempfile.TemporaryDirectory(prefix="lesstoken-publish-") as temp:
            root = Path(temp)
            for name in ("../private.txt", "/private.txt", "C:/private.txt", "dir\\private.txt"):
                with self.assertRaises(ValueError):
                    public.safe_file(root, name)

    def test_archive_only_contains_reviewed_files_and_verified_hashes(self):
        with tempfile.TemporaryDirectory(prefix="lesstoken-publish-") as temp:
            root = Path(temp)
            (root / "public.txt").write_text("reviewed", encoding="utf-8")
            (root / "auth.json").write_text("must stay private", encoding="utf-8")
            public.write_json(root / public.MANIFEST,
                              public.make_manifest(root, ["public.txt"], "0.1.0", "test"))
            target = root / "export.zip"
            public.archive(root, target, ["public.txt", public.MANIFEST])
            with public.zipfile.ZipFile(target) as package:
                self.assertNotIn("auth.json", package.namelist())
                self.assertEqual(package.read("public.txt"), b"reviewed")
            (root / "public.txt").write_text("changed", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                public.archive(root, root / "bad.zip", ["public.txt", public.MANIFEST])

    def test_packages_are_not_overwritten(self):
        with tempfile.TemporaryDirectory(prefix="lesstoken-publish-") as temp:
            root = Path(temp)
            (root / "a.txt").write_text("a", encoding="utf-8")
            public.write_json(root / public.MANIFEST, public.make_manifest(root, ["a.txt"], "0.1.0", "test"))
            target = root / "export.zip"
            public.archive(root, target, ["a.txt", public.MANIFEST])
            original = target.read_bytes()
            with self.assertRaises(FileExistsError):
                public.archive(root, target, ["a.txt", public.MANIFEST])
            self.assertEqual(target.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
