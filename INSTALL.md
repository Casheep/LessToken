# LessToken 安装

支持两种安装方式：让 Codex 按本文代为安装，或自己在普通 PowerShell / Windows Terminal 中安装。两者使用同一个脚本，不需要 API Key，也不要求事先把 codex 加到 PATH。

目前支持 Windows x64。安装 Codex 插件需要已有的 Codex 桌面端或 CLI；安装器会先查 PATH，再查桌面端在 `%LOCALAPPDATA%/OpenAI/Codex/bin` 中附带的 CLI。无需为了这个插件再安装一套 Codex。

将 Windows x64 发布 ZIP 解压到准备长期保留的位置。请使用包含 `plugins/lesstoken/bin/lesstoken.exe` 的发布包；GitHub 自动生成的源码 ZIP 没有预编译程序，需要先构建。

## 方式一：让 Codex 安装

将下面这段话发给 Codex，并将路径替换成实际解压路径：

> 请读取 LessToken 解压目录中的 INSTALL.md，按“给安装代理的步骤”完成默认安装、自检和安装结果检查。解压目录是：这里填写完整路径。不要启用文件批处理写操作。

例如将发布包解压到 `D:\Tools\LessToken`，可直接让 Codex 读取该目录里的 INSTALL.md。

### 给安装代理的步骤

用户要求安装本插件时，按顺序执行：

1. 以本文件所在目录为发布包根目录，检查 VERSION、安装脚本和 `plugins/lesstoken/bin/lesstoken.exe` 存在。不使用临时解压目录作为长期安装位置。
2. 执行 `powershell -NoProfile -ExecutionPolicy Bypass -File "发布包完整路径\scripts\verify.ps1"`。必须确认退出码为 0 且输出 PASS；这只是本地自检，不调用模型、不改变正常 Codex 配置。
3. 执行 `powershell -NoProfile -ExecutionPolicy Bypass -File "发布包完整路径\scripts\install.ps1"`，确认退出码为 0。安装器自动定位 Codex，支持普通命令行环境；特殊路径使用 -CodexPath，不修改系统 PATH。默认不加 -EnableFileWrites。
4. 检查安装输出中的实际插件目录，确认版本和 MCP 可执行文件存在、未设置 --allow-write。不要输出凭据或重写其他插件配置，不修改 AGENTS.md。
5. 告知用户安装结果、必须保留的目录，以及在实际项目中新开聊天。当前聊天尚未加载新工具时，不反复调用不存在的工具。在工具已加载的聊天中，可调用一次 lt_stats 确认项目目录及 file_writes_enabled=false，再做一次小范围读取验证。
6. Hooks 的审核信任由用户在 Codex 中完成；安装不等于自动优化已经生效。不伪造信任记录或使用绕过信任参数。只验证到哪一步，就报告到哪一步。

如果安装失败，检查并修复具体错误后再重试，不将失败报告成成功。卸载也使用随包脚本，不手动删除其他 Codex 配置。

## 方式二：普通命令行安装

在解压目录中打开 PowerShell、Windows Terminal 或命令提示符，依次运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

第一条应显示 PASS，第二条应显示 Installed。无需让 Codex 执行这些命令。

也可以在任意目录使用完整路径。以下示例需替换为实际解压目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Tools\LessToken\scripts\install.ps1"
```

若 Codex 位于其他位置，可明确指定它的路径；以下路径需要替换为实际位置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1 -CodexPath "D:\Apps\Codex\codex.exe"
```

默认安装 MCP、Skill 和待审核的 Hooks。仅安装 MCP/Skill 可加 -WithoutHooks；自定义数据目录可加 -DataDirectory。文件批处理写操作仅在用户明确需要时加 -EnableFileWrites，首次试用无需开启。

## 安装后

在实际项目中新开本地 Codex 聊天，按提示审核并信任 LessToken Hooks。CLI 可用 `/hooks` 查看；[官方信任流程](https://learn.chatgpt.com/docs/hooks#review-and-trust-hooks)要求审核实际定义，安装脚本不会替代这一步。

首次可发送“检查 LessToken 是否连接成功，确认项目根目录正确、file_writes_enabled=false”。之后正常使用即可，不必每次查询统计。MCP 由模型按任务选择，Hooks 自动处理支持的命令；并非所有操作都经过插件。

保留解压目录，MCP 使用其中程序的绝对路径。移动目录后需从新位置重新运行安装脚本。升级时使用新版发布包执行同样步骤。

## 卸载

在发布包根目录运行，Codex 代为卸载时也使用同一命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
```

卸载器同样能自动查找桌面端附带的 CLI，并支持 -CodexPath。卸载保留已保存的本地原文、表及审计数据。

## 只在命令行使用 LessToken 程序

如果只是使用原生输出精简程序，不安装 Codex 插件也可以；这些命令不需要 Codex、API Key 或额外运行时：

```powershell
.\plugins\lesstoken\bin\lesstoken.exe --version
.\plugins\lesstoken\bin\lesstoken.exe reduce --kind log --file .\app.log
```

将 app.log 换成实际日志路径。这个方式只处理显式交给程序的内容，不会自动接管 Codex。直接安装使用 Windows x64 发布包，其中已包含可执行程序，无需自行安装编译工具。
