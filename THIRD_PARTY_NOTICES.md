# Third-party material

The LessToken C++ implementation was written in this workspace. It does not link or bundle executables/libraries from RTK, TOK, Context Mode, Headroom, Token Optimizer, Serena, llmtrim, Token Harness, HarnessTrim or dsh-ops.

Selected dsh-ops native-core behaviours have been ported to C++20 for 0.2.0-beta.1. Original copyright (c) 2026 T-Auto and the dsh-ops contributors, MIT. Attribution, fixed source snapshot and port boundaries are in `third_party/dsh-ops/NOTICE.md`, with the original MIT text in `LICENSE.MIT`. Release packages retain both notices in `licenses/`. No upstream Rust worker or FastCtx code/runtime is shipped.

`third_party/nlohmann/json.hpp` is nlohmann/json v3.12.0 (MIT), with the original notice in `third_party/nlohmann/LICENSE.MIT`.
Source: https://github.com/nlohmann/json/tree/v3.12.0
SHA-256 of the vendored header: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.

Research checkouts, dependency environments, third-party release binaries and development reports are not part of the minimal source or runtime package. The selected dsh-ops source snapshot is recorded in its NOTICE.md above.

The packaged executable also includes the compiler's C/C++ runtime components under their upstream runtime-library licenses and exceptions. The local MinGW release package includes the available GCC runtime exception, headers and winpthreads license notices in `licenses/`, plus nlohmann/json's MIT notice. No upstream tools are bundled. This beta has not had an independent legal redistribution audit; no GitHub publication has been performed. A license for the original LessToken project itself has not yet been selected by its owner.
