"""Audit an explicit public file list and prepare local ZIPs. Never uploads.

No model calls, network requests, installation, credentials or Git writes.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CORE = (
    ".agents/plugins/marketplace.json", ".github/workflows/windows.yml", ".gitattributes", ".gitignore", "CMakeLists.txt",
    "README.md", "INSTALL.md", "PRIVACY.md", "THIRD_PARTY_NOTICES.md", "VERSION",
    "CHANGELOG.md", "DEVELOPMENT.md", "include/lesstoken.hpp",
    "plugins/lesstoken/.codex-plugin/plugin.json", "plugins/lesstoken/.mcp.json",
    "plugins/lesstoken/hooks/hooks.json", "plugins/lesstoken/hooks/invoke.ps1",
    "plugins/lesstoken/skills/lesstoken/SKILL.md",
    "scripts/build.ps1", "scripts/install.ps1", "scripts/resolve-codex.ps1",
    "scripts/uninstall.ps1", "scripts/verify.ps1", "scripts/prepare_github.py",
    "src/main.cpp", "src/core.cpp", "src/storage.cpp", "src/process.cpp",
    "src/mcp.cpp", "src/hook.cpp", "src/prompt.cpp", "src/query.cpp",
    "src/records.cpp", "src/batch.cpp", "src/native.hpp",
    "third_party/dsh-ops/LICENSE.MIT", "third_party/dsh-ops/NOTICE.md",
    "third_party/nlohmann/LICENSE.MIT", "third_party/nlohmann/json.hpp",
)
DEVELOPMENT = (
    "scripts/analyze_session_usage.py", "scripts/benchmark_optimization.py",
    "scripts/coding_evaluation.py", "scripts/compare_prompt_snapshots.py",
    "scripts/evaluate_codex.py", "tests/optimization.py", "tests/session_usage.py",
    "tests/evaluation.py", "tests/coding_evaluation.py", "tests/prompt_prefetch.py",
    "tests/hooks.py", "tests/publication.py", "docs/optimization.md",
    "docs/optimization-benchmark.json", "docs/e2e-round-01.json",
    "docs/e2e-round-02.json", "docs/e2e-native-validation.json",
    "docs/e2e-validation.md", "docs/github-readiness.md",
)
SOURCE = tuple(sorted(CORE + DEVELOPMENT))
MANIFEST = "PUBLICATION_MANIFEST.json"
PRIVATE = re.compile(r"(?i)(?:sk-(?:proj-)?[a-z0-9_-]{20,}|gh[pousr]_[a-z0-9]{20,}|github_pat_[a-z0-9_]{20,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)")
PERSONAL = re.compile(r"(?i)[a-z]:[\\/](?:Users|AFolder|AAApp)[\\/]")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_file(root, name):
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
        raise ValueError("Unsafe package path")
    path = root
    for part in relative.parts:
        path /= part
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("Package links are unsupported: " + name)
    if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Missing or outside public file: " + name)
    return path


def scan(name, data):
    # Only this explicitly selected, version-checked executable is binary.
    if name == "plugins/lesstoken/bin/lesstoken.exe":
        if not data.startswith(b"MZ"):
            raise ValueError("Invalid Windows executable")
        return
    text = data.decode("utf-8-sig")
    for category, pattern in (("possible credential", PRIVATE), ("personal path", PERSONAL)):
        if pattern.search(text):
            # Never print a potential secret value.
            raise ValueError(category + " in " + name)
    if name.endswith(".json"):
        json.loads(text)


def inventory(root, names):
    hashes = {}
    for name in sorted(names):
        path = safe_file(root, name)
        scan(name, path.read_bytes())
        hashes[name] = sha(path)
    return hashes


def make_manifest(root, names, version, kind):
    return {"version": version, "package_kind": kind,
            "files": inventory(root, names),
            "excluded": "Personal data, credentials, raw model logs, research, build caches and local installation settings. This manifest excludes itself."}


def audit(root=ROOT):
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.]+)?", version):
        raise ValueError("Invalid version")
    plugin = json.loads((root / "plugins/lesstoken/.codex-plugin/plugin.json").read_text())
    if plugin["version"] != version:
        raise ValueError("Plugin version mismatch")
    server = json.loads((root / "plugins/lesstoken/.mcp.json").read_text())["mcpServers"]
    if server != {"lesstoken": {"command": "${PLUGIN_ROOT}/bin/lesstoken.exe", "args": ["mcp"]}}:
        raise ValueError("MCP configuration is not portable or default read-only")
    hooks = json.loads((root / "plugins/lesstoken/hooks/hooks.json").read_text())["hooks"]
    if set(hooks) != {"UserPromptSubmit", "PreToolUse"}:
        raise ValueError("Unexpected hook registration")
    manifest = make_manifest(root, SOURCE, version, "source")
    saved = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    if saved != manifest:
        raise ValueError("Publication manifest is stale; review changes and use --write-manifest")
    scan(MANIFEST, (root / MANIFEST).read_bytes())
    return manifest


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def archive(folder, target, names):
    with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as package:
        for name in sorted(names):
            path = safe_file(folder, name)
            package.write(path, name)
    # Inspect archive bytes, not only the staging tree.
    with zipfile.ZipFile(target) as package:
        if sorted(package.namelist()) != sorted(names) or package.testzip() is not None:
            raise ValueError("Archive inventory or CRC mismatch")
        manifest = json.loads(package.read(MANIFEST))
        for name, expected in manifest["files"].items():
            data = package.read(name)
            scan(name, data)
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError("Archive hash mismatch: " + name)


def prepare(directory, compiler_licenses):
    manifest = audit()
    # Never overwrite or delete an earlier review artifact.
    directory.mkdir(parents=True, exist_ok=False)
    source = directory / "source"
    for name in SOURCE + (MANIFEST,):
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(safe_file(ROOT, name), target)
    version = manifest["version"]
    source_zip = directory / f"LessToken-{version}-source.zip"
    archive(source, source_zip, SOURCE + (MANIFEST,))
    zips = [source_zip]
    if compiler_licenses:
        binary_name = "plugins/lesstoken/bin/lesstoken.exe"
        binary = safe_file(ROOT, binary_name)
        actual = subprocess.check_output([str(binary), "--version"], text=True, timeout=10).strip()
        if actual != version:
            raise ValueError("Binary version mismatch; rebuild first")
        runtime = directory / "runtime"
        names = [name for name in CORE if name.startswith((".agents/", "plugins/"))
                 or name in ("README.md", "INSTALL.md", "PRIVACY.md", "THIRD_PARTY_NOTICES.md", "VERSION", "CHANGELOG.md",
                             "scripts/install.ps1", "scripts/uninstall.ps1", "scripts/resolve-codex.ps1", "scripts/verify.ps1")]
        for name in names + [binary_name]:
            target = runtime / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(safe_file(ROOT, name), target)
        names.append(binary_name)
        for group in ("gcc", "headers", "winpthreads"):
            paths = sorted((compiler_licenses / group).glob("*"))
            if not paths:
                raise ValueError("Missing runtime license group: " + group)
            for path in paths:
                if path.is_file():
                    name = "licenses/" + group + "/" + path.name
                    target = runtime / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(safe_file(compiler_licenses, group + "/" + path.name), target)
                    names.append(name)
        for old, new in (("third_party/dsh-ops/LICENSE.MIT", "dsh-ops-MIT.txt"),
                         ("third_party/dsh-ops/NOTICE.md", "dsh-ops-port-NOTICE.md"),
                         ("third_party/nlohmann/LICENSE.MIT", "nlohmann-json-MIT.txt")):
            name = "licenses/" + new
            shutil.copyfile(ROOT / old, runtime / name)
            names.append(name)
        write_json(runtime / MANIFEST, make_manifest(runtime, names, version, "windows-x64"))
        runtime_zip = directory / f"LessToken-{version}-windows-x64.zip"
        archive(runtime, runtime_zip, names + [MANIFEST])
        zips.append(runtime_zip)
    (directory / "SHA256SUMS.txt").write_text("".join(sha(path) + "  " + path.name + "\n" for path in zips), encoding="utf-8")
    print(json.dumps({"prepared": [path.name for path in zips], "uploaded": False}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-manifest", action="store_true", help="Refresh reviewed source hashes")
    parser.add_argument("--directory", type=Path, help="New local output directory; never overwritten")
    parser.add_argument("--compiler-licenses", type=Path, help="Compiler license root for optional Windows ZIP")
    args = parser.parse_args()
    if args.write_manifest:
        version = (ROOT / "VERSION").read_text().strip()
        write_json(ROOT / MANIFEST, make_manifest(ROOT, SOURCE, version, "source"))
    if args.compiler_licenses and not args.directory:
        parser.error("--compiler-licenses requires --directory")
    if args.directory:
        prepare(args.directory.resolve(), args.compiler_licenses.resolve() if args.compiler_licenses else None)
    else:
        print(json.dumps({"audit": "passed", "source_files": len(audit()["files"]), "uploaded": False}))


if __name__ == "__main__":
    main()
