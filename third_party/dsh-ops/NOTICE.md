Selected dsh-ops capabilities ported to C++ for LessToken.

Upstream: https://github.com/T-Auto/dsh-ops
Snapshot: 6e133320b4f7a81c289e05ff6afddc740cdef36e (0.3.0)
Original copyright: (c) 2026 T-Auto and the dsh-ops contributors.
Original native core and plugin licence: MIT (LICENSE.MIT in this directory).

Affected LessToken modules: src/query.cpp, src/records.cpp, src/batch.cpp,
src/native.hpp, and the corresponding MCP contracts/tests.

The selected behaviours are bounded search output modes, structured result
filtering/projection, and planned file operations with revision checks,
durable item states, and conservative recovery. They have been reimplemented
in C++20 and adapted to Codex/project-root scoping. This is a subset, not a
drop-in implementation of the DSH plugin protocol.

Differences include: regular-file-only batches; explicit startup write opt-in;
project-scoped records rather than DSH session scopes; existing LessToken tool
names; and no DSH runtime, Rust executable, FastCtx, Bash/MSYS, or PowerShell
runtime payload from upstream. The bundled Windows hook bridge and installation
scripts belong to LessToken. Development probes may test upstream binaries in
ignored .research directories, outside the shipped runtime.
