#include "lesstoken.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <set>
#include <stdexcept>

namespace lt {
CapturedFiles::~CapturedFiles() {
  if (directory.empty() || !path_text(directory.filename()).starts_with("lesstoken-process-")) return;
  std::error_code ec;
  if (directory.parent_path() == fs::temp_directory_path(ec)) fs::remove_all(directory, ec);
}
void stream_file(const fs::path& path, std::ostream& output) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot open captured stream.");
  std::array<char, 65536> bytes{};
  while (f) { f.read(bytes.data(), bytes.size()); output.write(bytes.data(), f.gcount()); }
}
static std::string safe_text(const std::string& text) {
  return json::parse(json(text).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
}
json byte_page(const fs::path& path, size_t offset, size_t bytes) {
  if (bytes < 1 || bytes > 16000) throw std::runtime_error("Use 1..16000 bytes per page.");
  auto total = fs::file_size(path);
  if (offset > total) throw std::runtime_error("Byte offset is past end of file.");
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read result.");
  f.seekg(static_cast<std::streamoff>(offset));
  std::string raw(std::min<uintmax_t>(bytes + 4, total - offset), '\0');
  f.read(raw.data(), raw.size()); raw.resize(static_cast<size_t>(f.gcount()));
  size_t skip = 0;
  while (skip < std::min<size_t>(raw.size(), 3) && (static_cast<unsigned char>(raw[skip]) & 0xc0) == 0x80) ++skip;
  raw.erase(0, skip); offset += skip;
  size_t n = std::min(bytes, raw.size());
  auto make = [&]() {
    while (n && n < raw.size() && (static_cast<unsigned char>(raw[n]) & 0xc0) == 0x80) --n;
    // A one-byte page still needs to advance over an entire UTF-8 scalar.
    if (!n && !raw.empty()) {
      n = 1;
      while (n < raw.size() && n < 4 && (static_cast<unsigned char>(raw[n]) & 0xc0) == 0x80) ++n;
    }
    return json{{"offset", offset}, {"next_offset", offset + n}, {"eof", offset + n >= total},
      {"total_bytes", total}, {"text", safe_text(raw.substr(0, n))}, {"encoding", "UTF-8; invalid bytes replaced in preview only"}};
  };
  auto result = make();
  while (result.dump().size() > 16500 && n > 1) { n /= 2; result = make(); }
  return result;
}
json search_file(const fs::path& path, const std::string& query, size_t limit) {
  if (query.empty() || query.size() > 1024 || limit < 1 || limit > 50) throw std::runtime_error("Invalid search arguments.");
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot search file.");
  std::array<char, 65536> block{};
  std::string overlap; auto needle = lower(query);
  size_t processed = 0, newlines = 0, last_match = 0, region_begin=0,region_end=0,merged=0; bool have_match = false, limited = false, hit_limit=false;
  json found = json::array();
  constexpr size_t scan_budget = 512 * 1024 * 1024;
  while (f && processed < scan_budget && found.size() < limit) {
    f.read(block.data(), block.size()); auto n = static_cast<size_t>(f.gcount()); if (!n) break;
    std::string text = overlap + std::string(block.data(), n);
    auto folded = lower(text); size_t base = processed - overlap.size();
    size_t start_line = newlines - static_cast<size_t>(std::count(overlap.begin(), overlap.end(), '\n')) + 1;
    size_t pos = 0;
    while ((pos = folded.find(needle, pos)) != std::string::npos && found.size() < limit) {
      size_t absolute = base + pos;
      if (!have_match || absolute > last_match) {
        size_t a=0,cursor=pos,b=text.size();
        for(int context=0;context<3;++context) {
          if(!cursor) {a=0;break;}
          auto previous=text.rfind('\n',cursor-1);if(previous==std::string::npos) {a=0;break;}
          a=previous+1;cursor=previous;
        }
        cursor=pos+needle.size();
        for(int context=0;context<9;++context) {auto next=text.find('\n',cursor);if(next==std::string::npos) {b=text.size();break;} b=next+1;cursor=b;}
        a=std::max(a,pos>400?pos-400:0);b=std::min(b,std::min(text.size(),pos+needle.size()+700));
        while (a < b && (static_cast<unsigned char>(text[a]) & 0xc0) == 0x80) ++a;
        while (b < text.size() && b > a && (static_cast<unsigned char>(text[b]) & 0xc0) == 0x80) --b;
        auto line = start_line + static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<long long>(pos), '\n'));
        size_t absolute_begin=base+a,absolute_end=base+b;
        if(!found.empty() && absolute_begin<=region_end && region_begin>=base && absolute_end-region_begin<=1600) {
          region_end=std::max(region_end,absolute_end);
          found.back()["text"]=safe_text(text.substr(region_begin-base,region_end-region_begin));++merged;
        } else {
          found.push_back({{"line", line}, {"offset", absolute}, {"text", safe_text(text.substr(a, b - a))}});
          region_begin=absolute_begin;region_end=absolute_end;
        }
        last_match = absolute; have_match = true;
      }
      pos += std::max<size_t>(1, needle.size());
    }
    if(found.size()>=limit && pos<text.size()) hit_limit=true;
    newlines += static_cast<size_t>(std::count(block.begin(), block.begin() + static_cast<long long>(n), '\n'));
    processed += n;
    overlap = text.substr(text.size() > 2048 ? text.size() - 2048 : 0);
  }
  limited = hit_limit || processed < fs::file_size(path);
  json result = {{"matches", found}, {"scanned_bytes", processed}, {"complete", !limited}, {"limit", limit},
    {"merged_overlaps",merged},{"note", "2 lines before, 8 after, byte-capped. Overlaps merge; limit counts regions. complete=false: more may exist. Page by offset."}};
  while (result.dump().size() > 17000 && !result["matches"].empty()) {
    result["matches"].erase(result["matches"].size() - 1);
    result["complete"] = false; result["response_limited"] = true;
  }
  return result;
}
std::string Store::save_file(const fs::path& file, const json& metadata) {
  const auto id = random_id();
  const auto target = root / "results" / (id + ".txt");
  try {
    fs::copy_file(file, target, fs::copy_options::none);
    write_file(root / "results" / (id + ".json"), metadata.dump());
  } catch (...) { std::error_code ec; fs::remove(target, ec); throw; }
  try { prune(512 * 1024 * 1024, 7, id); } catch (...) {} // housekeeping must not invalidate a completed capture
  return id;
}
static bool result_name(const std::string& name) {
  return name.size() == 32 && std::all_of(name.begin(), name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
json Store::prune(size_t max_bytes, size_t max_age_days, const std::string& keep_ref) {
  struct Entry { fs::path path; fs::file_time_type time; uintmax_t bytes; std::string id; };
  std::vector<Entry> entries; uintmax_t total = 0, removed = 0, removed_bytes = 0;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& item : fs::directory_iterator(root / "results")) {
    if (fs::is_symlink(item.symlink_status()) || !item.is_regular_file() || item.path().extension() != ".txt") continue;
    auto id = path_text(item.path().stem()); if (!result_name(id)) continue;
    auto size = item.file_size(); total += size;
    entries.push_back({item.path(), item.last_write_time(), size, id});
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
  for (const auto& entry : entries) {
    if (entry.id == keep_ref) continue;
    bool aged = now - entry.time > std::chrono::hours(24 * max_age_days);
    if (!aged && total <= max_bytes) continue;
    // References that another process has open can fail removal on Windows; keep them.
    std::error_code ec;
    if (fs::remove(entry.path, ec)) {
      ++removed; removed_bytes += entry.bytes; total -= entry.bytes;
      fs::remove(root / "results" / (entry.id + ".json"), ec);
    }
  }
  return {{"removed_results", removed}, {"removed_bytes", removed_bytes}, {"remaining_bytes", total},
    {"quota_bytes", max_bytes}, {"over_quota", total > max_bytes}, {"keep_days", max_age_days},
    {"note", "Only owned result files are removed; newest active capture may exceed the soft quota."}};
}

static std::string large_preview(const fs::path& file, const std::string& kind) {
  std::ifstream f(file, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read output preview.");
  std::deque<std::pair<size_t,std::string>> before, tail;
  std::array<std::map<size_t,std::string>,4> groups;
  size_t number = 0, diagnostics = 0, severe = 0, clipped = 0, after = 0;
  int after_priority = 0;
  auto retain=[&](int priority,size_t n,const std::string& text) {
    auto& group=groups[static_cast<size_t>(priority)];group[n]=text;
    // Bounded first/last windows, so early warnings cannot consume all error slots.
    if(group.size()>96) {auto middle=group.begin();std::advance(middle,48);group.erase(middle);}
  };
  std::string line; line.reserve(8192);
  auto finish = [&]() {
    ++number;
    const auto low = lower(line);
    int priority=0;
    for(const auto& n:{"warn","警告"}) if(low.find(n)!=std::string::npos) priority=1;
    for(const auto& n:{"expected","actual","期望","实际"}) if(low.find(n)!=std::string::npos) priority=2;
    for(const auto& n:{"error","fatal","fail","assert","exception","traceback","错误","失败","异常"})
      if(low.find(n)!=std::string::npos) priority=3;
    if(number<=6) retain(0,number,line);
    if(after) {retain(after_priority,number,line);--after;}
    if (priority) {
      ++diagnostics;
      severe+=priority==3;
      for(const auto& v:before) retain(priority,v.first,v.second);
      retain(priority,number,line);
      // Keep enough following lines for multi-line errors; never downgrade an active error window.
      if(priority>=after_priority || !after) {after_priority=priority;after=priority==3?8:3;}
    }
    before.emplace_back(number, line); if (before.size() > 2) before.pop_front();
    tail.emplace_back(number, line); if (tail.size() > 10) tail.pop_front();
    line.clear();
  };
  char c;
  while (f.get(c)) {
    if (c == '\n') finish();
    else if (line.size() < 2048) line += c;
    else if (line.size() == 2048) { line += " [line preview clipped; use offset paging]"; ++clipped; }
  }
  if (!line.empty()) finish();
  for (const auto& v : tail) retain(0,v.first,v.second);
  std::map<size_t,std::string> selected;
  size_t selection_bytes = 0;
  for(int priority=3;priority>=0;--priority) {
    const auto& group=groups[static_cast<size_t>(priority)];auto left=group.begin(),right=group.end();bool front=true;
    while(left!=right) {
      auto chosen=left;if(front) ++left;else chosen=--right;front=!front;
      const auto& [n,text]=*chosen;auto cost=text.size()+32;
      if(!selected.contains(n) && selection_bytes+cost<8200) {selected[n]=text;selection_bytes+=cost;}
    }
  }
  std::string out = "[Bounded " + kind + " preview; " + std::to_string(number) + " lines; " + std::to_string(diagnostics) + " diagnostic lines; "+std::to_string(severe)+" error/failure lines; errors prioritized over warnings]\n";
  size_t last = 0, shown = 0;
  for (const auto& [n,text] : selected) {
    if (last && n > last + 1) out += "[lines " + std::to_string(last + 1) + ".." + std::to_string(n - 1) + " omitted]\n";
    out += std::to_string(n) + ": " + safe_text(text) + "\n"; last = n; ++shown;
  }
  out += "[" + std::to_string(number - shown) + " lines omitted; " + std::to_string(clipped) + " long-line previews clipped. Search or page the full original.]\n";
  return out;
}
std::string file_view(const fs::path& path, const std::string& kind, Store& store, bool track) {
  auto size = fs::file_size(path);
  if (size <= input_limit) {
    auto raw = read_file(path); auto view = reduce(raw, kind);
    auto estimate=std::min(raw.size(),view.text.size()+256);
    if (view.text.size() <= 16000 && (raw.size()<=17500 || raw.size()-estimate>=std::max<size_t>(512,raw.size()/5)))
      return display(raw, view, store, json::object(), track);
  }
  auto ref = store.save_file(path, {{"kind", kind}, {"input_bytes", size}});
  auto out = "[LessToken original ref=" + ref + "; complete original preserved]\n" + large_preview(path, kind);
  if (track) store.record(ref, size, out.size(), "bounded-file-preview");
  return out;
}
json Store::page(const std::string& id, size_t offset, size_t bytes) {
  if (!result_name(id)) throw std::runtime_error("Invalid result reference.");
  return byte_page(result_path(root,id), offset, bytes);
}
}
