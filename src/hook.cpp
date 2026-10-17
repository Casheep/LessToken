#include "lesstoken.hpp"
#include <cstdlib>
#include <sstream>
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
  if (event.value("hook_event_name", std::string()) != "PostToolUse") return json::object();
  const auto name = event.value("tool_name", std::string());
  if (name != "Bash" && name != "exec_command" && name != "shell_command") return json::object();
  if (!event.contains("tool_response")) return json::object();
  const auto& response = event["tool_response"];
  // The opt-in test audit stores structure only, never command output or credentials.
  if (const char* audit = std::getenv("LESSTOKEN_HOOK_AUDIT")) {
    json shape{{"event", "PostToolUse"}, {"tool", name}, {"response_type", response.type_name()}};
    if (response.is_object()) {
      shape["keys"] = json::array();
      for (auto it = response.begin(); it != response.end(); ++it) shape["keys"].push_back(it.key());
    }
    write_file(from_utf8(audit), shape.dump());
  }
  std::string text;
  if (response.is_string()) text = response.get<std::string>();
  else if (response.is_object()) {
    if (response.contains("output") && response["output"].is_string()) text = response["output"].get<std::string>();
    else if (response.contains("stdout") && response["stdout"].is_string()) {
      text = response["stdout"].get<std::string>();
      if (response.contains("stderr") && response["stderr"].is_string()) text += "\n[stderr]\n" + response["stderr"].get<std::string>();
    } else if (response.contains("content") && response["content"].is_array()) {
      for (const auto& block : response["content"])
        if (block.value("type", std::string()) == "text") text += block.value("text", std::string());
        else return json::object();
    }
  }
  if (text.size() < 4096 || text.find('\0') != std::string::npos || text.starts_with("[LessToken")) return json::object();
  auto command = event.value("tool_input", json::object()).value("command", std::string());
  for (const auto& flag : {"--porcelain", "--json", "-z", "ConvertTo-Json"})
    if (command.find(flag) != std::string::npos) return json::object();
  if (response.is_object() && response.contains("session_id") && !response["session_id"].is_null()) return json::object();
  const bool tests = text.find(" PASSED ") != std::string::npos || text.find("... Passed ") != std::string::npos;
  auto reduction = reduce(text, tests ? "test" : "log");
  if (reduction.mode == "passthrough" || reduction.text.size() > 6500) return json::object();
  Store store;
  bool upstream_truncated = text.find("truncated output") != std::string::npos || text.find("tokens truncated") != std::string::npos;
  auto ref = store.save(text, {{"source", "PostToolUse"}, {"tool", name}, {"upstream_truncated", upstream_truncated}});
  std::string preview = "[LessToken: tool already completed; visible output folded; ref=" + ref + "]\n";
  if (upstream_truncated) preview += "The host already truncated this output. This ref preserves only the received portion; use source-file reading for missing details.\n";
  if (response.is_object() && response.contains("exit_code")) preview += "exit_code=" + response["exit_code"].dump() + "\n";
  preview += reduction.text;
  preview += "\nUse lt_result or lesstoken search/fetch to retrieve the stored tool output.\n";
  if (preview.size() >= text.size() || text.size()-preview.size()<std::max<size_t>(512,text.size()/5)) return json::object();
  store.record(ref, text.size(), preview.size(), "post-tool-use");
  return {{"continue", false}, {"stopReason", preview}};
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
  auto command = input.value("command", input.value("cmd", std::string()));
  if (command.find("lesstoken.exe") != std::string::npos) return json::object();
  // Build/test commands are only auto-wrapped when the host already grants
  // unrestricted execution. Default/plan modes retain their original command
  // approval path; this hook never promotes a build into an approved read.
  if (event.value("permission_mode", std::string()) == "bypassPermissions"
      && command.size() < 16000 && command.find_first_of("$`|;&<>\r\n()@#") == std::string::npos
      && command.find("''") == std::string::npos) {
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
          auto path = confined(from_utf8(base), from_utf8(operand));
          auto ext = lower(path_text(path.extension()));
          if (fs::is_regular_file(path) && (ext == ".log" || ext == ".out" || ext == ".json" || ext == ".txt")) {
            std::ifstream f(path, std::ios::binary); char prefix[4]{}; f.read(prefix, 4);
            bool binary = std::find(prefix, prefix + f.gcount(), '\0') != prefix + f.gcount();
            if (!binary) {
              auto ps_quote = [](std::string s) { std::string r = "'"; for (char c : s) r += c == '\'' ? "''" : std::string(1,c); return r + "'"; };
              auto replacement = "& " + ps_quote(path_text(executable_path())) + " reduce --kind " + (ext == ".json" ? "json" : "log") + " --file " + ps_quote(path_text(path));
              input[input.contains("command") ? "command" : "cmd"] = replacement;
              return {{"hookSpecificOutput", {{"hookEventName", "PreToolUse"}, {"permissionDecision", "allow"}, {"permissionDecisionReason", "LessToken literal project text read"}, {"updatedInput", input}}}};
            }
          }
        } catch (...) {} // unresolved/out-of-project input reaches the host unchanged
      }
    }
  }
  if (command.size() > 512 || command.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -") != std::string::npos) return json::object();
  std::istringstream s(command); std::vector<std::string> words;
  for (std::string word; s >> word;) words.push_back(word);
  if (words.size() < 2 || words[0] != "git") return json::object();
  if (words[1] == "status") {
    if (words.size() != 2) return json::object();
  } else if (words[1] == "log") {
    if (words.size() == 3 && words[2] != "--oneline") return json::object();
    if (words.size() > 3) return json::object();
  } else return json::object();
  // updatedInput requires protocol-level allow in current Codex. Restrict this
  // opt-in rewrite to known permission modes and narrow inspection commands.
  // No write, test/build, path operand, machine format, redirection or pipeline qualifies.
  std::string rewritten;
#ifdef _WIN32
  if (input.contains("shell")) {
    auto sh = lower(input["shell"].get<std::string>());
    if (sh.find("powershell") == std::string::npos && sh.find("pwsh") == std::string::npos) return json::object();
  }
  auto exe = path_text(executable_path());
  std::string ps = "'";
  for (char c : exe) ps += c == '\'' ? "''" : std::string(1, c);
  ps += "'";
  rewritten = "& " + ps + " run --kind log --";
#else
  rewritten = quote_arg(executable_path().string()) + " run --kind log --";
#endif
  for (const auto& w : words) rewritten += " " + w;
  // Preserve original execution-context fields. Refuse rather than erase policy fields.
  auto key = input.contains("command") ? "command" : "cmd";
  input[key] = rewritten;
  return {{"hookSpecificOutput", {{"hookEventName", "PreToolUse"}, {"permissionDecision", "allow"},
    {"permissionDecisionReason", "LessToken opt-in bounded read-only git wrapper"}, {"updatedInput", input}}}};
}
}
