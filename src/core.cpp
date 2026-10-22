#include "lesstoken.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
    if (result.size() > limit) throw std::runtime_error("Input exceeds 16 MiB limit; request a bounded read instead.");
  }
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
static bool protected_line(const std::string& line) {
  auto s = lower(line);
  static const std::vector<std::string> needles = {"error", "fatal", "warn", "fail", "exception", "assert", "traceback", "panic", "expected", "actual", "错误", "失败", "异常", "警告", "期望", "实际"};
  return std::any_of(needles.begin(), needles.end(), [&](const auto& n) { return s.find(n) != std::string::npos; });
}
static std::string exact_block_table(const std::vector<std::string>& source,size_t start,size_t width,size_t repeats) {
  // Store every variable value explicitly. No timestamp normalization, inferred
  // numeric ranges, reordered rows or omitted values are permitted.
  if(width<=16) return {};
  const auto& first=source[start];size_t prefix=first.size(),suffix=first.size(),raw_bytes=0;
  for(size_t i=0;i<width;++i) {
    const auto& line=source[start+i];raw_bytes+=line.size()+1;
    size_t n=0;while(n<std::min(prefix,line.size()) && first[n]==line[n]) ++n;prefix=n;
    n=0;while(n<std::min(suffix,line.size()) && first[first.size()-n-1]==line[line.size()-n-1]) ++n;suffix=n;
  }
  while(prefix && prefix<first.size() && (static_cast<unsigned char>(first[prefix])&0xc0)==0x80) --prefix;
  for(size_t i=0;i<width;++i) suffix=std::min(suffix,source[start+i].size()-prefix);
  while(suffix && (static_cast<unsigned char>(first[first.size()-suffix])&0xc0)==0x80) --suffix;
  if(prefix+suffix<24) return {};
  json values=json::array();
  for(size_t i=0;i<width;++i) {
    const auto& line=source[start+i];values.push_back(line.substr(prefix,line.size()-prefix-suffix));
  }
  try {
    auto table=json{{"prefix",first.substr(0,prefix)},{"values",values},{"suffix",first.substr(first.size()-suffix)},
      {"repeat",repeats}}.dump();
    auto result="[Exact repeated source block: each line = prefix + value + suffix; values stay in order; repeat the entire block repeat times]\n"+table;
    return result.size()+128<raw_bytes?result:std::string();
  } catch(const json::exception&) {return {};}
}
Reduction reduce(const std::string& input, const std::string& kind, size_t budget) {
  if (kind == "raw" || kind == "code" || input.size() < 512 || input.find('\0') != std::string::npos)
    return {input, "passthrough", 0};
  if (kind == "json") {
    auto parsed = json::parse(input, nullptr, false);
    if (!parsed.is_discarded()) {
      // No array sampling, value truncation, floating-point reformatting or key dedup:
      // whitespace-only compaction preserves every JSON token byte-for-byte.
      std::string compact;
      bool in_string = false, escape = false;
      for (char c : input) {
        if (in_string) {
          compact += c;
          if (escape) escape = false;
          else if (c == '\\') escape = true;
          else if (c == '"') in_string = false;
        } else if (c == '"') { compact += c; in_string = true; }
        else if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
      }
      if (compact.size() + 256 < input.size()) return {compact, "json-whitespace-only", 0};
    }
    return {input, "passthrough", 0};
  }
  if (kind != "log" && kind != "test") return {input, "passthrough", 0};
  auto source = lines(input);
  std::vector<std::string> dedup;
  bool folded_block = false, folded_table = false;
  for (size_t i = 0; i < source.size();) {
    size_t j = i + 1;
    while (j < source.size() && source[j] == source[i]) ++j;
    if (j - i > 1) {
      dedup.push_back(source[i]);
      dedup.push_back("[previous line repeated " + std::to_string(j - i - 1) + " more times]");
      i = j;
      continue;
    }
    // Exact adjacent blocks only: no timestamp stripping, semantic summaries or
    // cross-gap deduplication. Retain order and multiplicity, and leave diagnostic
    // blocks expanded so separate failures and their context remain visible.
    size_t best_width = 0, best_repeats = 0, best_gain = 128;
    // Build/progress groups often span more than 16 lines. Keep a fixed bound,
    // and still compare every original line before encoding a repeat.
    for (size_t width = 2; width <= 64 && i + 2 * width <= source.size(); ++width) {
      if (source[i] != source[i + width]) continue;
      size_t block_bytes = 0; bool diagnostic = false;
      for (size_t k = 0; k < width; ++k) {
        block_bytes += source[i + k].size() + 1;
        diagnostic = diagnostic || protected_line(source[i + k]);
      }
      if (diagnostic) continue;
      size_t repeats = 1;
      while (i + (repeats + 1) * width <= source.size()
          && std::equal(source.begin() + static_cast<long long>(i),
              source.begin() + static_cast<long long>(i + width),
              source.begin() + static_cast<long long>(i + repeats * width))) ++repeats;
      auto marker = "[previous " + std::to_string(width) + " lines repeated " + std::to_string(repeats - 1) + " more times]";
      const auto removed = (repeats - 1) * block_bytes;
      if (repeats > 1 && removed > marker.size() + 1 + best_gain) {
        best_gain = removed - marker.size() - 1; best_width = width; best_repeats = repeats;
      }
    }
    if (best_width) {
      // Test-pass omission works on physical lines, so keep its existing format.
      auto table=kind=="log"?exact_block_table(source,i,best_width,best_repeats):std::string();
      if(!table.empty()) {dedup.push_back(std::move(table));folded_table=true;}
      else {
        for (size_t k = 0; k < best_width; ++k) dedup.push_back(source[i + k]);
        dedup.push_back("[previous " + std::to_string(best_width) + " lines repeated " + std::to_string(best_repeats - 1) + " more times]");
      }
      i += best_width * best_repeats; folded_block = true;
    } else {dedup.push_back(source[i]); ++i;}
  }
  std::vector<bool> keep(dedup.size(), true);
  bool bounded = false;
  if (input.size() > budget && kind == "test") {
    // Drop only explicit passed-test lines. Failure blocks and arbitrary context survive.
    for (size_t i = 0; i < dedup.size(); ++i) {
      const auto& line = dedup[i];
      bool passed = line.find(" PASSED ") != std::string::npos || line.ends_with(" PASSED")
        || line.find("... Passed ") != std::string::npos
        || (line.starts_with("test ") && line.find(" ... ") != std::string::npos && line.ends_with("ok"));
      if (passed && !protected_line(line)) keep[i] = false;
    }
    // Preserve neighbouring details around every diagnostic, including Chinese errors.
    for (size_t i = 0; i < dedup.size(); ++i) if (protected_line(dedup[i])) {
      for (size_t j = i > 2 ? i - 2 : 0; j < std::min(dedup.size(), i + 5); ++j) keep[j] = true;
    }
    bounded = true;
  }
  std::ostringstream result;
  size_t omitted = 0;
  for (size_t i = 0; i < dedup.size(); ++i) {
    if (keep[i]) result << dedup[i] << '\n';
    else ++omitted;
  }
  if (omitted) result << "[" << omitted << " passed-test lines omitted; full original available]\n";
  auto text = result.str();
  // Avoid wrappers that cost more than the original small result.
  if (text.size() + 256 >= input.size()) return {input, "passthrough", 0};
  return {text, bounded && omitted ? "test-pass-fold" : folded_table ? "exact-affix-block-fold" : folded_block ? "exact-block-fold" : "exact-line-fold", omitted};
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
  size_t input = 0, shown = 0, events = 0;
  for (const auto& e : fs::directory_iterator(root / "events")) {
    if (e.path().extension() != ".json" || !e.is_regular_file()) continue;
    auto value = json::parse(read_file(e.path()), nullptr, false);
    if (value.is_discarded()) continue;
    input += value.value("input_bytes", size_t(0)); shown += value.value("shown_bytes", size_t(0)); ++events;
  }
  return {{"events", events}, {"actual_input_bytes", input}, {"actual_shown_bytes", shown},
    {"net_bytes_removed", static_cast<long long>(input) - static_cast<long long>(shown)},
    {"billing_measurement", false}, {"note", "Stage-level byte counters include intermediate hook/wrapper outputs and retrieval overhead; stages may overlap. Not provider-visible totals, billed tokens, or subscription quota."}};
}
std::string display(const std::string& raw, const Reduction& reduction, Store& store, const json& metadata, bool track) {
  if (reduction.mode == "passthrough") {
    if (track) store.record("", raw.size(), raw.size(), reduction.mode);
    return raw;
  }
  const bool lossless=reduction.mode=="exact-line-fold" || reduction.mode=="exact-block-fold" || reduction.mode=="exact-affix-block-fold" || reduction.mode=="json-whitespace-only";
  const std::string guidance=lossless?"; lossless=true; all source content represented; original retrieval optional":"; fetch/search for complete original";
  auto suffix="\n[LessToken " + reduction.mode + "; ref=" + std::string(32,'0') + guidance + "]\n";
  auto visible=reduction.text.size()+suffix.size();
  // Include reference overhead. Marginal reductions are not worth losing context.
  if (visible >= raw.size() || raw.size()-visible < std::max<size_t>(512,raw.size()/5)) {
    if (track) store.record("", raw.size(), raw.size(), "low-gain-passthrough");
    return raw;
  }
  // Save before emitting a lossy view. Save failure propagates to the caller's raw fallback.
  auto ref = store.save(raw, metadata);
  auto result = reduction.text + "\n[LessToken " + reduction.mode + "; ref=" + ref + guidance + "]\n";
  if (track) store.record(ref, raw.size(), result.size(), reduction.mode);
  return result;
}
json doctor() {
  return {{"name", "LessToken"}, {"version", version}, {"executable", path_text(executable_path())},
    {"data_directory", path_text(data_root())}, {"local_only", true}, {"api_key_required", false},
    {"mcp_transport", "stdio"}, {"host_verification", "Requires installation and hook approval in the target Codex host."},
    {"hook_scope", "Explicit small-file prompt snapshots; PowerShell literal log/JSON and git reads; supported test/build argv in bypassPermissions mode. No post-output hook. Unsupported/nested calls pass through."},
    {"measurement", "Actual bytes only. No billed-token or subscription savings claim."}};
}
}
