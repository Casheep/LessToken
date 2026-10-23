#include "lesstoken.hpp"
#include <algorithm>
#include <sstream>

namespace lt {
bool lossless_mode(const std::string& mode) {
  return mode=="passthrough" || mode=="exact-line-fold" || mode=="exact-block-fold" ||
    mode=="exact-affix-block-fold" || mode=="json-whitespace-only";
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
  // Literal encoder markers in source would be ambiguous after another fold.
  // Already processed output and adversarial marker-like logs stay unchanged.
  for(const auto& line:source)
    if(line.starts_with("[previous ") || line.starts_with("[Exact repeated source block:") || line.starts_with("[LessToken "))
      return {input,"passthrough",0};
  std::vector<signed char> diagnostics(source.size(),-1);
  auto is_diagnostic=[&](size_t index) {
    auto& cached=diagnostics[index];
    if(cached<0) cached=protected_line(source[index])?1:0;
    return cached!=0;
  };
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
    // Long build/tool groups also enter conversation history. The bounded scan
    // admits wider exact repeats; every original line still has to match.
    for (size_t width = 2; !is_diagnostic(i) && width <= 512 && i + 2 * width <= source.size(); ++width) {
      if (source[i] != source[i + width]) continue;
      size_t block_bytes = 0; bool diagnostic = false;
      for (size_t k = 0; k < width; ++k) {
        block_bytes += source[i + k].size() + 1;
        diagnostic = diagnostic || is_diagnostic(i + k);
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
}
