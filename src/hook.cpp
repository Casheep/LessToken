#include "lesstoken.hpp"
#include <cstdlib>
#include <set>
#include <fstream>
#include <algorithm>

namespace lt {
json post_hook(const json& event) {
  if (const char* audit = std::getenv("LESSTOKEN_HOOK_AUDIT")) {
    json shape{{"event", event.value("hook_event_name", std::string())}, {"tool", event.value("tool_name", std::string())}, {"event_keys", json::array()}};
    for (auto it = event.begin(); it != event.end(); ++it) shape["event_keys"].push_back(it.key());
    write_file(from_utf8(audit), shape.dump());
  }
  // Codex PostToolUse cannot replace output. continue=false stops the agent,
  // so legacy --post invocations must be harmless no-ops.
  return json::object();
}

json hook(const json& event, bool explicit_opt_in) {
  auto enabled = std::getenv("LESSTOKEN_HOOK_REWRITE");
  if (!explicit_opt_in && (!enabled || std::string(enabled) != "1")) return json::object();
  static const std::set<std::string> modes = {"default", "acceptEdits", "plan", "dontAsk", "bypassPermissions"};
  if (!modes.contains(event.value("permission_mode", std::string()))) return json::object();
  // Do not modify nested functions.exec JavaScript, MCP arguments or arbitrary shells.
  auto name = event.value("tool_name", std::string());
  if (name != "Bash" && name != "exec_command" && name != "shell_command") return json::object();
  if (event.value("hook_event_name", std::string("PreToolUse")) != "PreToolUse") return json::object();
  if (!event.contains("tool_input") || !event["tool_input"].is_object()) return json::object();
  auto input = event["tool_input"];
  if (input.contains("sandbox_permissions") || input.contains("prefix_rule") || input.contains("env")) return json::object();
#ifdef _WIN32
  // Every emitted replacement uses PowerShell syntax. Check before all rewrite
  // paths, including build/test and literal reads, rather than just git.
  if (input.contains("shell")) {
    auto shell = lower(path_text(from_utf8(input["shell"].get<std::string>()).filename()));
    if (shell != "powershell" && shell != "powershell.exe" && shell != "pwsh" && shell != "pwsh.exe") return json::object();
  }
#endif
  auto command = input.value("command", input.value("cmd", std::string()));
  if (command.find("lesstoken.exe") != std::string::npos) return json::object();
  // Build/test commands are only auto-wrapped when the host already grants
  // unrestricted execution. Default/plan modes retain their original command
  // approval path; this hook never promotes a build into an approved read.
  if (event.value("permission_mode", std::string()) == "bypassPermissions"
      && command.size() < 16000 && command.find_first_of("$`|;&<>\r\n()@#") == std::string::npos
      && command.find("''") == std::string::npos && command.find("\"\"") == std::string::npos) {
    std::vector<std::string> args; std::string arg; char q = 0;
    for (char c : command) {
      if (q) { if (c == q) q = 0; else arg += c; }
      else if (c == '\'' || c == '"') q = c;
      else if (c == ' ' || c == '\t') { if (!arg.empty()) { args.push_back(arg); arg.clear(); } }
      else arg += c;
    }
    if (!arg.empty()) args.push_back(arg);
    if (!q && args.size() >= 2) {
      auto program = lower(path_text(from_utf8(args[0]).filename()));
      if (program.ends_with(".exe")) program.resize(program.size() - 4);
      std::string kind;
      if (program == "ctest") kind = "test";
      if (program == "cmake" && args[1] == "--build") kind = "log";
      if (program == "dotnet" && (args[1] == "test" || args[1] == "build")) kind = args[1] == "test" ? "test" : "log";
      if ((program == "python" || program == "python3") && args.size() >= 3 && args[1] == "-m" && args[2] == "pytest") kind = "test";
      if ((program == "python" || program == "python3" || program == "node")
          && (args[1].ends_with(".py") || args[1].ends_with(".js")) && lower(args[1]).find("test") != std::string::npos) {
        try { confined(from_utf8(event.at("cwd").get<std::string>()), from_utf8(args[1])); kind = "test"; } catch (...) {}
      }
      if (!kind.empty()) {
        auto quote_ps = [](const std::string& s) { std::string r = "'"; for (char c : s) r += c == '\'' ? "''" : std::string(1,c); return r + "'"; };
        std::string replacement = "& " + quote_ps(path_text(executable_path())) + " run --kind " + kind + " --";
        for (const auto& a : args) replacement += " " + quote_ps(a);
        input[input.contains("command") ? "command" : "cmd"] = replacement;
        return {{"hookSpecificOutput", {{"hookEventName", "PreToolUse"}, {"permissionDecision", "allow"},
          {"permissionDecisionReason", "LessToken build/test output wrapper in existing unrestricted mode"}, {"updatedInput", input}}}};
      }
    }
  }
  // A literal project-log read is reduced before the host's own truncation.
  // This is not an arbitrary shell rewrite: one cmdlet, one existing file,
  // no expressions/pipelines/wildcards, and UTF-8 text inside the active root.
  if (command.starts_with("Get-Content ") && command.find_first_of("$`|;&<>\r\n") == std::string::npos) {
    std::vector<std::string> tokens; std::string token; char quote = 0;
    for (size_t i = 0; i < command.size(); ++i) {
      char c = command[i];
      if (quote) { if (c == quote) quote = 0; else token += c; }
      else if (c == '\'' || c == '"') quote = c;
      else if (c == ' ' || c == '\t') { if (!token.empty()) { tokens.push_back(token); token.clear(); } }
      else token += c;
    }
    if (!token.empty()) tokens.push_back(token);
    if (!quote && tokens.size() >= 2 && tokens.size() <= 3 && (tokens.size() == 2 || tokens[1] == "-LiteralPath" || tokens[1] == "-Path")) {
      auto operand = tokens.back();
      if (operand.find_first_of("*?[]'") == std::string::npos) {
        auto base = event.value("cwd", std::string());
        if (input.contains("workdir") && input["workdir"].is_string()) base = input["workdir"].get<std::string>();
        try {
          auto project = from_utf8(event.at("cwd").get<std::string>());
          auto path = confined(project, confined(project, from_utf8(base)) / from_utf8(operand));
          auto ext = lower(path_text(path.extension()));
          if (fs::is_regular_file(path) && (ext == ".log" || ext == ".out" || ext == ".json" || ext == ".txt")) {
            const auto size=fs::file_size(path);
            if(size<4096) return json::object();
            std::ifstream f(path, std::ios::binary); char prefix[4]{}; f.read(prefix, 4);
            bool binary = std::find(prefix, prefix + f.gcount(), '\0') != prefix + f.gcount();
            if(f.gcount()>=2 && ((static_cast<unsigned char>(prefix[0])==0xff && static_cast<unsigned char>(prefix[1])==0xfe) ||
              (static_cast<unsigned char>(prefix[0])==0xfe && static_cast<unsigned char>(prefix[1])==0xff))) binary=true;
            if (!binary) {
              auto ps_quote = [](std::string s) { std::string r = "'"; for (char c : s) r += c == '\'' ? "''" : std::string(1,c); return r + "'"; };
              auto replacement = "& " + ps_quote(path_text(executable_path())) + " reduce --kind " + (ext == ".json" ? "json" : "log") + " --file " + ps_quote(path_text(path));
              // For bounded known inputs, include the added command text and
              // reference notice in the gain check. Never pre-run build/tests.
              if(size<=65536) {
                auto raw=read_file(path,65536);
                const auto extra=replacement.size()>command.size()?replacement.size()-command.size():0;
                if(!worthwhile_display(raw,reduce(raw,ext==".json"?"json":"log"),extra)) return json::object();
              }
              input[input.contains("command") ? "command" : "cmd"] = replacement;
              return {{"hookSpecificOutput", {{"hookEventName", "PreToolUse"}, {"permissionDecision", "allow"}, {"permissionDecisionReason", "LessToken literal project text read"}, {"updatedInput", input}}}};
            }
          }
        } catch (...) {} // unresolved/out-of-project input reaches the host unchanged
      }
    }
  }
  // Git status/log have no useful reduction here. Wrapping their already short
  // output only adds command/history overhead.
  return json::object();
}
}
