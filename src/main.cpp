#include "lesstoken.hpp"
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace lt {
static std::string stdin_text() {
  std::string result; char buf[8192];
  while (std::cin) {
    std::cin.read(buf, sizeof(buf)); result.append(buf, static_cast<size_t>(std::cin.gcount()));
    if (result.size() > input_limit) throw std::runtime_error("Input exceeds 16 MiB limit.");
  }
  return result;
}
int main_impl(const std::vector<std::string>& argv) {
  if (argv.empty() || argv[0] == "--help" || argv[0] == "help") {
    std::cout << "LessToken " << version << "\n"
      "  reduce --kind log|test|json|raw|code [--file PATH]\n"
      "  run --kind log|test|json|raw [--timeout-ms N] -- EXECUTABLE ARG...\n"
      "  fetch REF [START=1] [COUNT=100]\n  search REF QUERY\n"
      "  page REF [OFFSET=0] [BYTES=12000]\n  prune [MAX_MIB=512] [DAYS=7]\n"
      "  mcp [--root PROJECT] [--allow-write]\n  ops [--root PROJECT] [--allow-write] < request.json\n  hook [--prompt|--rewrite-readonly|--post|--context]\n  stats\n  doctor\n";
    return 0;
  }
  if (argv[0] == "--version") { std::cout << version << '\n'; return 0; }
  if (argv[0] == "doctor") { std::cout << doctor().dump(2) << '\n'; return 0; }
  if (argv[0] == "hook") {
    // A malformed/unsupported hook must never break the original host call.
    try {
      auto event = json::parse(stdin_text());
      auto output = argv.size() == 2 && argv[1] == "--prompt" ? prompt_hook(event)
        : argv.size() == 2 && argv[1] == "--context" ? context_hook(event)
        : argv.size() == 2 && argv[1] == "--post" ? post_hook(event)
        : hook(event, argv.size() == 2 && argv[1] == "--rewrite-readonly");
      if (!output.empty()) std::cout << output.dump() << '\n';
    } catch (...) {}
    return 0;
  }
  if (argv[0] == "mcp" || argv[0] == "ops") {
    auto root = fs::current_path();bool allow_write=false;
    for(size_t i=1;i<argv.size();++i) {
      if(argv[i]=="--root" && i+1<argv.size()) root=from_utf8(argv[++i]);
      else if(argv[i]=="--allow-write") allow_write=true;
      else throw std::runtime_error("Use mcp/ops [--root PROJECT] [--allow-write]");
    }
    root=fs::canonical(root);
    if(argv[0]=="mcp") return mcp_main(root,allow_write);
    auto request=json::parse(stdin_text());Store store;
    std::cout<<mcp_call(request.at("tool").get<std::string>(),request.value("arguments",json::object()),root,store,allow_write)<<'\n';return 0;
  }
  if (argv[0] == "stats") { Store store; std::cout << store.stats().dump(2) << '\n'; return 0; }
  if (argv[0] == "prune") {
    Store store; auto max_mib = argv.size() > 1 ? std::stoull(argv[1]) : 512;
    auto days = argv.size() > 2 ? std::stoull(argv[2]) : 7;
    if (max_mib > 1024 * 1024 || days > 36500) throw std::runtime_error("Invalid retention limits.");
    std::cout << store.prune(max_mib * 1024 * 1024, days).dump(2) << '\n'; return 0;
  }
  if (argv[0] == "page" && argv.size() >= 2) {
    Store store; auto out = store.page(argv[1], argv.size() > 2 ? std::stoull(argv[2]) : 0, argv.size() > 3 ? std::stoull(argv[3]) : 12000).dump();
    store.record(argv[1], 0, out.size() + 1, "byte-page"); std::cout << out << '\n'; return 0;
  }
  if (argv[0] == "fetch" && argv.size() >= 2) {
    Store store;
    auto output = store.fetch(argv[1], argv.size() > 2 ? std::stoull(argv[2]) : 1, argv.size() > 3 ? std::stoull(argv[3]) : 100);
    store.record(argv[1], 0, output.size(), "fetch"); std::cout << output; return 0;
  }
  if (argv[0] == "search" && argv.size() == 3) {
    Store store;
    auto output = store.search(argv[1], argv[2]).dump(2);
    store.record(argv[1], 0, output.size() + 1, "search"); std::cout << output << '\n'; return 0;
  }
  if (argv[0] != "reduce" && argv[0] != "run") throw std::runtime_error("Unknown command.");
  std::string kind = "raw", file;
  std::vector<std::string> command;
  int timeout_ms = 120000;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "--" && argv[0] == "run") { command.assign(argv.begin() + static_cast<long long>(i + 1), argv.end()); break; }
    if (argv[i] == "--kind" && i + 1 < argv.size()) kind = argv[++i];
    else if (argv[i] == "--file" && i + 1 < argv.size() && argv[0] == "reduce") file = argv[++i];
    else if (argv[i] == "--timeout-ms" && i + 1 < argv.size() && argv[0] == "run") {
      timeout_ms = std::stoi(argv[++i]); if (timeout_ms < 1 || timeout_ms > 3600000) throw std::runtime_error("Invalid timeout.");
    } else throw std::runtime_error("Unknown or incomplete option: " + argv[i]);
  }
  if (kind != "raw" && kind != "code" && kind != "log" && kind != "test" && kind != "json") throw std::runtime_error("Unknown output kind.");
  if (argv[0] == "reduce") {
    std::shared_ptr<CapturedFiles> stdin_file;
    if (file.empty()) {
      stdin_file = std::make_shared<CapturedFiles>();
      stdin_file->directory = fs::temp_directory_path() / ("lesstoken-process-" + random_id());
      fs::create_directory(stdin_file->directory);
      auto path = stdin_file->directory / "stdin";
      std::ofstream captured(path, std::ios::binary);
      if (!captured) throw std::runtime_error("Cannot spool input.");
      captured << std::cin.rdbuf(); captured.close();
      file = path_text(path);
    }
    if (!file.empty() && kind != "raw" && kind != "code") {
      try { Store store; std::cout << file_view(from_utf8(file), kind, store); }
      catch (const std::exception& e) { std::cerr << "LessToken bypass: " << e.what() << '\n'; stream_file(from_utf8(file), std::cout); }
      return 0;
    }
    if (!file.empty() && fs::file_size(from_utf8(file)) > input_limit) { stream_file(from_utf8(file), std::cout); return 0; }
    auto raw = file.empty() ? stdin_text() : read_file(from_utf8(file));
    try { Store store; std::cout << display(raw, reduce(raw, kind), store); }
    catch (const std::exception& e) { std::cerr << "LessToken bypass: " << e.what() << '\n'; std::cout << raw; }
    return 0;
  }
  auto result = execute(command, timeout_ms); // Execute exactly once; never retry after reduction failure.
  try {
    Store store;
    if (kind == "raw" || kind == "code") { stream_file(result.stdout_file, std::cout); store.record("", result.stdout_bytes, result.stdout_bytes, "passthrough"); }
    else std::cout << file_view(result.stdout_file, kind, store);
  } catch (const std::exception& e) { std::cerr << "LessToken bypass: " << e.what() << '\n'; stream_file(result.stdout_file, std::cout); }
  // Preserve stderr separately, in full. Diagnostics never disappear into stdout.
  stream_file(result.stderr_file, std::cerr);
  if (kind != "raw" && kind != "code") std::cout << "\n[LessToken process_exit_code=" << result.exit_code << "]\n";
  if (result.timed_out) std::cerr << "\nLessToken: subprocess timed out; owned descendant tree terminated.\n";
  return result.exit_code;
}
}
int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY); _setmode(_fileno(stderr), _O_BINARY);
  int n = 0; auto wide = CommandLineToArgvW(GetCommandLineW(), &n);
  if (!wide) return 2;
  for (int i = 1; i < n; ++i) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0'); WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), len, nullptr, nullptr); s.pop_back(); args.push_back(s);
  }
  LocalFree(wide); (void)argc; (void)argv;
#else
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
  try { return lt::main_impl(args); }
  catch (const lt::ToolError& e) {std::cout<<lt::json{{"status","error"},{"error",e.detail}}.dump()<<'\n';return 2;}
  catch (const std::exception& e) { std::cerr << "LessToken: " << e.what() << '\n'; return 2; }
}
