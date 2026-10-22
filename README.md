# LessToken

为 Codex 减少重复输出和不必要的工具往返的本地插件，在尽可能不降低回答质量的前提下减少 token 消耗。核心使用 C++20，支持 Windows x64 的 Codex 桌面端和 CLI。

当前版本：**0.2.0-beta.12**。

## 能做什么

- 精简重复的日志和测试输出，保留错误、相关上下文及可补查的原文。
- 合并搜索结果中的重叠上下文，减少重复内容和补读。
- 在本地直接筛选 JSON 数组，只向模型返回所需记录和字段。
- 按需读取、搜索和分页查询大文件。
- 对明确点名的小文件检查，提前在本地准备完整证据，减少模型取文件的往返。
- 自动复用已核对且仍在上下文中的相同文件内容，避免重复附加。

综合聊天测试中，总 token 用量相比关闭插件减少约 **44%**；同时，针对长聊天的优化在专项测试中额外减少约 **36.3%** 的 token 用量；单次历史上下文压缩的总 token 用量减少约 **31.2%**。

安装后，Hooks 自动处理支持的命令，Codex 也可按任务选择 MCP 工具。无需每次手动提醒，但并非所有操作都会经过插件。

## 安装

### 普通命令行

下载 [Windows x64 安装包](https://github.com/Casheep/LessToken/releases/download/v0.2.0-beta.12/LessToken-0.2.0-beta.12-windows-x64.zip)，解压到一个长期保留的目录，在该目录打开 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

安装器会自动查找 PATH 或 Codex 桌面端附带的 CLI，无需手动配置 PATH。安装后在项目中新开聊天，按提示审核并信任 LessToken Hooks。

**请保留解压目录。** 插件从中启动，移动后需要重新运行安装脚本。默认不启用文件批处理写操作。

### 让 Codex 安装

告诉 Codex：

> 请读取 LessToken 解压目录中的 INSTALL.md，按文档完成默认安装和验证。目录是：这里填写完整路径。不要启用文件批处理写操作。

特殊路径、安装选项和验证步骤见 [INSTALL.md](INSTALL.md)。

## 卸载

在解压目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
```

卸载保留已保存的本地数据。

## 参考项目

- [dsh-ops](https://github.com/T-Auto/dsh-ops)
- [RTK](https://github.com/rtk-ai/rtk)
- [TOK](https://github.com/MantisWare/tok)
- [Context Mode](https://github.com/mksglu/context-mode)
- [Headroom](https://github.com/headroomlabs-ai/headroom)
- [Token Optimizer](https://github.com/alexgreensh/token-optimizer)
- [Serena](https://github.com/oraios/serena)
- [llmtrim](https://github.com/fkiene/llmtrim)
- [Token Harness](https://github.com/giuliastro/token-harness)
- [HarnessTrim](https://github.com/giuliastro/HarnessTrim)
