#include "lesstoken.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace lt {
std::string read_file(const fs::path& path, size_t limit) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read file: " + path_text(path));
  std::string result;
  std::array<char, 8192> buf{};
  while (f) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    result.append(buf.data(), static_cast<size_t>(f.gcount()));
    if (result.size() > limit) throw std::runtime_error("Input exceeds " + std::to_string(limit) + " byte limit; request a bounded read instead.");
  }
  if (f.bad()) throw std::runtime_error("Cannot finish reading file: " + path_text(path));
  return result;
}
void write_file(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("Cannot write file: " + path_text(path));
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) throw std::runtime_error("Cannot finish writing file.");
}
std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> result;
  size_t start = 0, end;
  while ((end = text.find('\n', start)) != std::string::npos) {
    result.push_back(text.substr(start, end - start)); start = end + 1;
  }
  if (start < text.size()) result.push_back(text.substr(start));
  return result;
}
std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}
std::string random_id() {
  std::array<unsigned char, 16> b{};
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw std::runtime_error("System random generator failed.");
#else
  std::random_device r;
  for (auto& c : b) c = static_cast<unsigned char>(r());
#endif
  std::ostringstream s;
  for (auto c : b) s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  return s.str();
}
fs::path data_root() {
#ifdef _WIN32
  if (const wchar_t* p = _wgetenv(L"LESSTOKEN_DATA")) return fs::absolute(fs::path(p));
  if (const wchar_t* p = _wgetenv(L"LOCALAPPDATA")) return fs::path(p) / "LessToken";
#else
  if (const char* p = std::getenv("LESSTOKEN_DATA")) return fs::absolute(from_utf8(p));
  if (const char* p = std::getenv("XDG_STATE_HOME")) return from_utf8(p) / "lesstoken";
  if (const char* p = std::getenv("HOME")) return from_utf8(p) / ".local/state/lesstoken";
#endif
  return fs::temp_directory_path() / "lesstoken";
}
fs::path executable_path() {
#ifdef _WIN32
  std::wstring b(32768, L'\0');
  auto n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
  if (!n || n == b.size()) throw std::runtime_error("Cannot locate executable.");
  b.resize(n); return fs::path(b);
#else
  return fs::read_symlink("/proc/self/exe");
#endif
}
fs::path confined(const fs::path& root, const fs::path& input) {
  auto r = fs::canonical(root);
  auto p = fs::canonical(input.is_absolute() ? input : r / input);
  auto ri = r.begin(), pi = p.begin();
  for (; ri != r.end(); ++ri, ++pi) {
    if (pi == p.end()) throw std::runtime_error("Path is outside the configured project root.");
#ifdef _WIN32
    if (lower(path_text(*ri)) != lower(path_text(*pi))) throw std::runtime_error("Path is outside the configured project root.");
#else
    if (*ri != *pi) throw std::runtime_error("Path is outside the configured project root.");
#endif
  }
  return p;
}
Store::Store(fs::path path) : root(fs::absolute(std::move(path))) {
  fs::create_directories(root / "results");
  fs::create_directories(root / "events");
}
fs::path result_path(const fs::path& root, const std::string& id) {
  if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); }))
    throw std::runtime_error("Invalid result reference.");
  restore_context_archive(root,id);
  return confined(root / "results", root / "results" / (id + ".txt"));
}
std::string Store::save(const std::string& raw, const json& metadata) {
  auto id = random_id();
  write_file(root / "results" / (id + ".txt"), raw);
  write_file(root / "results" / (id + ".json"), metadata.dump());
  try { prune(512 * 1024 * 1024, 7, id); } catch (...) {}
  return id;
}
std::string Store::fetch(const std::string& id, size_t start, size_t count) {
  if (!start || count > 500 || !count) throw std::runtime_error("Use start >= 1 and 1 <= count <= 500.");
  auto path=result_path(root,id);
  if(!fs::is_regular_file(path)) throw ToolError("RESULT_UNAVAILABLE","Original result is not a regular file.");
  std::ifstream f(path, std::ios::binary);
  if(!f) throw ToolError("IO_ERROR","Cannot open original result.");
  std::ostringstream out;
  size_t current = 1, shown = 0, line_bytes = 0; std::string line; char c;
  auto emit=[&]() {
    if(line.size()>12000) {
      size_t end=12000;
      while(end && (static_cast<unsigned char>(line[end]) & 0xc0)==0x80) --end;
      line.resize(end);
    }
    out << current << ": " << line;
    if(line_bytes>line.size()) out << " [long line clipped; use byte offset paging]";
    out << '\n';++shown;
  };
  while (f.get(c)) {
    // Keep one lookahead byte so a clipped UTF-8 character can be removed whole.
    if (c != '\n') { ++line_bytes; if (current >= start && line.size() < 12001) line += c; continue; }
    if (current >= start) emit();
    if (shown >= count || out.tellp() > 14000) break;
    ++current; line.clear(); line_bytes = 0;
  }
  if(f.bad()) throw ToolError("IO_ERROR","Cannot finish reading original result.");
  if (f.eof() && line_bytes && current >= start && shown < count) emit();
  return out.str();
}
json Store::search(const std::string& id, const std::string& query, size_t context, size_t limit) {
  if (query.empty() || query.size() > 1024 || context > 10 || limit < 1 || limit > 50) throw std::runtime_error("Invalid bounded search arguments.");
  auto result = search_file(result_path(root, id), query, limit);
  result["ref"] = id;
  return result;
}
void Store::record(const std::string& ref, size_t input_bytes, size_t shown_bytes, const std::string& kind) {
  auto event = json{{"ref", ref}, {"input_bytes", input_bytes}, {"shown_bytes", shown_bytes}, {"kind", kind}, {"billing_measurement", false}};
  write_file(root / "events" / (random_id() + ".json"), event.dump());
}
json Store::stats() {
  size_t input = 0, shown = 0, events = 0, skipped = 0;
  const auto maximum=static_cast<size_t>(std::numeric_limits<long long>::max());
  for (const auto& e : fs::directory_iterator(root / "events")) {
    if (e.path().extension() != ".json") continue;
    try {
      if(fs::is_symlink(e.symlink_status()) || !e.is_regular_file()) {++skipped;continue;}
      // Events are small counters. Corrupt, partial or concurrently removed
      // records must not break statistics or wrap their signed byte difference.
      auto value=json::parse(read_file(e.path(),4096),nullptr,false);
      if(!value.is_object()) {++skipped;continue;}
      auto counter=[&](const char* key) {
        const auto& n=value.at(key);
        if(!n.is_number_integer() || (!n.is_number_unsigned() && n.get<long long>()<0))
          throw std::runtime_error("Invalid event counter.");
        auto bytes=n.get<uint64_t>();
        if(bytes>maximum) throw std::runtime_error("Event counter exceeds supported range.");
        return static_cast<size_t>(bytes);
      };
      auto a=counter("input_bytes"),b=counter("shown_bytes");
      if(input>maximum-a || shown>maximum-b) {++skipped;continue;}
      input+=a;shown+=b;++events;
    } catch(const std::exception&) {++skipped;}
  }
  return {{"events", events}, {"actual_input_bytes", input}, {"actual_shown_bytes", shown},
    {"skipped_events",skipped},{"complete",skipped==0},
    {"net_bytes_removed", static_cast<long long>(input) - static_cast<long long>(shown)},
    {"billing_measurement", false}, {"note", "Stage-level byte counters include intermediate hook/wrapper outputs and retrieval overhead; stages may overlap. Not provider-visible totals, billed tokens, or subscription quota."}};
}
static std::string reduction_suffix(const std::string& mode,const std::string& ref) {
  const std::string guidance=lossless_mode(mode)?"; lossless=true; all source content represented; original retrieval optional":"; fetch/search for complete original";
  return "\n[LessToken " + mode + "; ref=" + ref + guidance + "]\n";
}
bool worthwhile_display(const std::string& raw,const Reduction& reduction,size_t extra_bytes) {
  if(reduction.mode=="passthrough" || reduction.text.size()>=raw.size()) return false;
  const auto room=raw.size()-reduction.text.size(),suffix=reduction_suffix(reduction.mode,std::string(32,'0')).size();
  if(suffix>=room || extra_bytes>=room-suffix) return false;
  return room-suffix-extra_bytes>=std::max<size_t>(512,raw.size()/5);
}
std::string display(const std::string& raw, const Reduction& reduction, Store& store, const json& metadata, bool track) {
  if (reduction.mode == "passthrough") {
    if (track) store.record("", raw.size(), raw.size(), reduction.mode);
    return raw;
  }
  // Include reference overhead. Marginal reductions are not worth losing context.
  if (!worthwhile_display(raw,reduction)) {
    if (track) store.record("", raw.size(), raw.size(), "low-gain-passthrough");
    return raw;
  }
  // Save before emitting a lossy view. Save failure propagates to the caller's raw fallback.
  auto ref = store.save(raw, metadata);
  auto result = reduction.text + reduction_suffix(reduction.mode,ref);
  if (track) store.record(ref, raw.size(), result.size(), reduction.mode);
  return result;
}
json doctor() {
  return {{"name", "LessToken"}, {"version", version}, {"executable", path_text(executable_path())},
    {"data_directory", path_text(data_root())}, {"local_only", true}, {"api_key_required", false},
    {"mcp_transport", "stdio"}, {"host_verification", "Requires installation and hook approval in the target Codex host."},
    {"hook_scope", "Explicit small-file prompt snapshots; adaptive PowerShell literal log/JSON reads; supported test/build argv in bypassPermissions mode. Brief or unhelpful known reads pass through. No post-output hook. Unsupported/nested calls pass through."},
    {"measurement", "Actual bytes only. No billed-token or subscription savings claim."}};
}
}
