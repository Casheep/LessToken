# 本地开发与 GitHub 准备

支持 Windows x64，核心使用 C++20。需要 CMake、Ninja 和 GCC/MinGW 或 MSVC；构建脚本可查找 CLion 自带的 GCC 工具。Python 3 仅供开发测试，运行安装包不需要 Python。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
ctest --test-dir build --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1
python .\scripts\prepare_github.py
```

如果 `ctest` 不在 PATH，使用 CMake 安装目录中 `ctest.exe` 的完整路径。本地测试不调用模型、不安装插件、不修改登录或钩子信任。MinGW 功能判定器需要 GCC；MSVC 构建不运行这一个编译器专用测试组。

`.github/workflows/windows.yml` 在推送、PR 或手动触发时使用 Windows/MSVC 构建和本地测试，权限仅为读取仓库。Action 固定到提交哈希。流程不调用模型、不安装插件、不推送或发布；当前仅配置，尚未在 GitHub 运行。

`PUBLICATION_MANIFEST.json` 是当前明确允许公开的源码文件及 SHA-256 清单，排除自身。审阅过代码变更后，运行 `python scripts/prepare_github.py --write-manifest` 更新清单，再重新执行检查。不要用更新清单代替代码审查。

```powershell
python .\scripts\prepare_github.py --directory .\dist\github-ready
```

输出目录必须不存在。源码 ZIP 由明确的文件列表生成；不会包含 `.git`、`.research`、编译目录、本机安装配置、原始模型日志或个人数据。额外传入 `--compiler-licenses` 指定实际编译器的许可证目录时，会准备带程序的 Windows ZIP，并带上实际文件哈希和第三方说明。脚本不上传、不提交、不打标签，也不安装插件。

源码目录的 `.mcp.json` 必须保留 `${PLUGIN_ROOT}` 和默认只读参数。安装器会在解压后的安装目录内写入绝对路径；不要从已安装目录直接发布源码。普通用户使用 Windows ZIP，GitHub 自动生成的源码 ZIP 不包含程序。

## 评测与公开范围

常规回归测试不使用模型。`scripts/evaluate_codex.py` 默认只准备样本；显式 `--run` 才调用 Codex，使用现有登录并消耗额度。模型、推理档位、基线和权限必须明确指定。原始结果仅保存在忽略的 `.research` 中。完整任务对照不能用输出字节比例代替。

`scripts/analyze_session_usage.py` 只读取明确指定的本地会话记录，不把原文写入统计报告。`scripts/benchmark_optimization.py` 的编码器计数需要开发环境额外安装 `tiktoken`；这不是运行插件的依赖。

发布前重新检查版本、测试、哈希、ZIP 内容、第三方说明和安装行为。当前工作在 `codex/` 分支，远程推送、PR、标签和 Release 均需用户另行指示。当前尚未选择原创代码的开源许可证；第三方许可证不替代项目许可证。
