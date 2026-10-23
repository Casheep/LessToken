#include "lesstoken.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace lt {
static constexpr size_t response_limit = 18000;
static json schema(json properties, json required = json::array()) {
  return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
static json integer(size_t value, size_t max) {
  return {{"type", "integer"}, {"minimum", 1}, {"maximum", max}, {"default", value}};
}
static json preview_integer(size_t value,size_t maximum,size_t minimum=1) {
  return {{"type","integer"},{"minimum",minimum},{"default",value},
    {"description","Preview caps at "+std::to_string(maximum)+"; larger requests return explicit cap metadata. Check completeness/pagination."}};
}
static json tools(bool allow_write) {
  auto text = json{{"type", "string"}};
  auto file_slice=schema({{"path",text},{"start",integer(1,10000000)},{"count",integer(20,500)},
    {"kind",{{"type","string"},{"enum",{"auto","raw","code","log","test","json"}}}}},{"path"});
  auto read_schema=schema({{"path",text},{"files",{{"type","array"},{"items",{{"oneOf",json::array({text,file_slice})}}},{"minItems",1},{"maxItems",16}}},
    {"start",integer(1,10000000)},{"count",integer(100,500)},
    {"kind",{{"type","string"},{"enum",{"auto","raw","code","log","test","json"}},{"default","auto"}}},
    {"offset",{{"type","integer"},{"minimum",0}}},{"bytes",integer(12000,16000)}});
  read_schema["oneOf"]=json::array({{{"required",{"path"}}},{{"required",{"files"}}}});
  auto search_schema=schema({{"query",text},{"queries",{{"type","array"},{"items",text},{"minItems",1},{"maxItems",8}}},
    {"path",text},{"limit",preview_integer(10,100)},
    {"mode",{{"type","string"},{"enum",{"content","files","count","summary"}},{"default","content"}}},
    {"glob",{{"type","array"},{"items",text},{"maxItems",32}}},
    {"before",preview_integer(1,40,0)},{"after",preview_integer(2,40,0)}});
  search_schema["oneOf"]=json::array({{{"required",{"query"}}},{{"required",{"queries"}}}});
  auto list=json::array({
    {{"name", "lt_read"}, {"description", "Read project text: path or files[1..16] of path strings or {path,start?,count?,kind?}. Batch defaults to 20 lines/file; per-file fields override shared options. next_file continues a bounded batch. lossless=true views represent all content; original retrieval is optional. Code slices are exact; clipped views have refs."},
      {"inputSchema", read_schema},
      {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}},
    {{"name", "lt_search"}, {"description", "Literal project search: query or queries[1..8]. Multiple queries share original source windows and report independent counts. Set before/after (0..40); overlaps merge. Prefer mode=files/count/summary when text is unnecessary. line/lines identify hits; limit counts hits, not groups. glob supports include/!exclude. Check complete/scan_complete and clipped."},
      {"inputSchema", search_schema},
      {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}},
    {{"name", "lt_result"}, {"description", "Raw refs: query, start/count or byte offset/bytes. Tables: put exactly one of path (project JSON-array file), items or source_ref; include equals/fields to receive the selected page immediately. Prefer path over read then put when the file and filters are known. Page refs with unchanged equals/fields; offsets follow filtered rows. release deletes tables only. j- refs page batch audits. Tables expire in 24h and are project-scoped."},
      {"inputSchema", schema({{"ref", text}, {"query", text}, {"start", integer(1, 10000000)}, {"count", integer(100, 500)},
        {"offset", {{"type", "integer"}, {"minimum", 0}}}, {"bytes", integer(12000, 16000)},
        {"action",{{"type","string"},{"enum",{"put","page","release"}}}},
        {"items",{{"type","array"},{"items",{{"type","object"}}},{"maxItems",10000}}},{"source_ref",text},{"path",text},
        {"equals",{{"type","object"}}},{"fields",{{"type","array"},{"items",text},{"maxItems",32}}},
        {"limit",preview_integer(100,512)},{"max_bytes",{{"type","integer"},{"minimum",512},{"maximum",16000}}}})},
      {"annotations", {{"readOnlyHint", false}, {"destructiveHint",false},{"openWorldHint", false}}}},
    {{"name","lt_batch"},{"description","Regular-file copy/move/remove only, project-relative / paths. plan requires 1..128 independent operations; resume executes that exact ref, never redoing completed/failed/unknown items. No overwrites, directories, links, or Git metadata. run/resume require server --allow-write. Page item states via lt_result. Use only for user-authorized file changes."},
      {"inputSchema",schema({{"action",{{"type","string"},{"enum",{"plan","run","resume"}}}},{"ref",text},
        {"operations",{{"type","array"},{"minItems",1},{"maxItems",128},{"items",schema({{"op",{{"type","string"},{"enum",{"copy","move","remove"}}}},{"path",text},{"to",text}},{"op","path"})}}},
        {"protect",{{"type","array"},{"items",text},{"maxItems",128}}}},{"action"})},
      {"annotations",{{"readOnlyHint",false},{"destructiveHint",true},{"idempotentHint",false},{"openWorldHint",false}}}},
    {{"name", "lt_stats"}, {"description", "Local actual-byte counters, including display and retrieval overhead. Not billed tokens or subscription quota."},
      {"inputSchema", schema(json::object())}, {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}}
  });
  if(!allow_write) for(auto it=list.begin();it!=list.end();++it) if((*it)["name"]=="lt_batch") {list.erase(it);break;}
  return list;
}
static std::string bounded_text(const std::string& text) {
  if (text.size() <= response_limit) return text;
  size_t n = response_limit;
  while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
  return text.substr(0, n) + "\n[Response byte limit reached. Narrow query or request fewer lines.]";
}
static size_t number(const json& args, const std::string& key, size_t def, size_t maximum) {
  if (!args.contains(key)) return def;
  if (!args[key].is_number_unsigned() && !args[key].is_number_integer()) throw std::runtime_error("Expected positive integer: " + key);
  auto value = args[key].get<long long>();
  if (value < 1 || static_cast<unsigned long long>(value) > maximum) throw std::runtime_error("Out-of-range argument: " + key);
  return static_cast<size_t>(value);
}
static std::string inferred_read_kind(const fs::path& path) {
  auto ext=lower(path_text(path.extension()));
  return ext==".log" || ext==".out"?"log":ext==".json"?"json":"raw";
}
static std::string slice(const fs::path& path, size_t start, size_t count, size_t& consumed) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read project file.");
  std::string line, out;
  size_t current = 0, scanned = 0;
  // Bounded memory even for a malicious single gigantic line.
  char c;
  while (f.get(c)) {
    if (++scanned > input_limit) throw std::runtime_error("Scan exceeds 16 MiB; narrow the file.");
    if (c != '\n') {
      if (current + 1 < start) continue;
      if (line.size() < response_limit + 1) line += c;
      else throw std::runtime_error("Line exceeds response limit; use byte offset paging.");
      continue;
    }
    ++current;
    if (current >= start) { consumed += line.size() + 1; out += std::to_string(current) + ": " + line + "\n"; }
    line.clear();
    if (current >= start + count - 1 || out.size() > response_limit) return bounded_text(out);
  }
  if (f.bad()) throw std::runtime_error("Cannot finish reading project file.");
  if (!line.empty() && ++current >= start) { consumed += line.size(); out += std::to_string(current) + ": " + line + "\n"; }
  return bounded_text(out);
}
std::string mcp_call(const std::string& name, const json& args, const fs::path& root, Store& store, bool allow_write) {
  if (name == "lt_stats") { auto stats = store.stats(); stats["project_root"] = path_text(root);stats["data_directory"]=path_text(store.root);stats["file_writes_enabled"]=allow_write;return stats.dump(); }
  if (name == "lt_search") {auto out=project_query(root,args).dump();store.record("",0,out.size(),"search-response");return out;}
  if (name == "lt_batch") return batch_tool(root,args,allow_write).dump();
  if (name == "lt_read") {
    if(args.contains("files")) {
      if(args.contains("path") || args.contains("offset") || args.contains("bytes") || !args["files"].is_array() || args["files"].empty() || args["files"].size()>16) throw ToolError("INVALID_ARGUMENT","Use path or files[1..16]; batch reads do not accept byte offsets");
      json result={{"results",json::array()},{"next_file",0},{"complete",false}};
      for(size_t i=0;i<args["files"].size();++i) {
        json item=json::object();
        try {
          const auto& entry=args["files"][i];auto single=args;single.erase("files");
          if(entry.is_string()) single["path"]=entry;
          else if(entry.is_object()) {
            for(auto it=entry.begin();it!=entry.end();++it) {
              if(it.key()!="path" && it.key()!="start" && it.key()!="count" && it.key()!="kind") throw ToolError("INVALID_ARGUMENT","File slices accept only path/start/count/kind");
              single[it.key()]=it.value();
            }
            if(!entry.contains("path")) throw ToolError("INVALID_ARGUMENT","File slice requires path");
          } else throw ToolError("INVALID_ARGUMENT","Batch entries must be path strings or file-slice objects");
          if(!single["path"].is_string()) throw ToolError("INVALID_ARGUMENT","File path must be a string");
          auto path=single["path"].get<std::string>();
          if(path.empty() || path.size()>4096) throw ToolError("INVALID_ARGUMENT","Use a nonempty file path of at most 4096 bytes");
          if(std::any_of(path.begin(),path.end(),[](unsigned char c){return c<32 || c==127;})) throw ToolError("INVALID_ARGUMENT","File paths must not contain control characters");
          item["path"]=path;
          auto kind=single.value("kind",std::string("auto"));
          if(kind=="auto") kind=inferred_read_kind(confined(root,from_utf8(path)));
          if(!single.contains("count") && kind!="log" && kind!="test" && kind!="json") single["count"]=20;
          auto text=mcp_call(name,single,root,store,allow_write);
          item["text"]=text;
          auto standalone=json{{"results",json::array({item})},{"next_file",i+1},{"complete",false}};
          // Keep complete selected evidence whenever a response can carry it.
          // A later file can continue in the next batch instead of forcing a
          // second retrieval of text that would fit here without clipping.
          if(standalone.dump(-1,' ',false,json::error_handler_t::replace).size()>17000) {
            auto ref=store.save(text,{{"kind","selected-read-view"},{"path",path}});
            size_t n=2000;while(n && n<text.size() && (static_cast<unsigned char>(text[n])&0xc0)==0x80) --n;
            text=text.substr(0,n)+"\n[Selected view clipped; ref="+ref+"; fetch this view or request a narrower original slice.]";
          }
          item["text"]=text;
        } catch(const ToolError& e) {item["error"]=e.detail;} catch(const std::exception& e) {item["error"]=ToolError("READ_FAILED",std::string(e.what()).substr(0,500)).detail;}
        result["results"].push_back(item);
        result["next_file"]=i+1;
        if(result.dump(-1,' ',false,json::error_handler_t::replace).size()>17000) {result["results"].erase(result["results"].size()-1);result["next_file"]=i;break;}
      }
      result["complete"]=result["next_file"].get<size_t>()==args["files"].size();
      return result.dump(-1,' ',false,json::error_handler_t::replace);
    }
    auto path = confined(root, from_utf8(args.at("path").get<std::string>()));
    auto kind = args.value("kind", std::string("auto"));
    if (args.contains("offset")) {
      if (!args["offset"].is_number_unsigned() && (!args["offset"].is_number_integer() || args["offset"].get<long long>() < 0)) throw std::runtime_error("Invalid offset.");
      auto out = byte_page(path, args["offset"].get<size_t>(), number(args,"bytes",12000,16000)).dump();
      store.record("",0,out.size(),"file-byte-page"); return out;
    }
    {
      std::ifstream source(path, std::ios::binary); char prefix[512];
      source.read(prefix, sizeof(prefix)); std::string head(prefix, static_cast<size_t>(source.gcount()));
      if (head.find('\0') != std::string::npos || head.starts_with("\xff\xfe") || head.starts_with("\xfe\xff"))
        throw std::runtime_error("UTF-16/binary file: convert a copy to UTF-8 for text tools, or use explicit byte offset paging (invalid UTF-8 replaced in previews). Original file is unchanged.");
    }
    if (kind == "auto") kind=inferred_read_kind(path);
    size_t consumed = 0;
    if (kind == "raw" || kind == "code") {
      auto out = slice(path, number(args, "start", 1, 10000000), number(args, "count", 100, 500), consumed);
      store.record("", consumed, out.size(), "bounded-read"); return out;
    }
    if (kind != "log" && kind != "test" && kind != "json") throw std::runtime_error("Unknown read kind.");
    if (args.contains("start") || args.contains("count")) throw std::runtime_error("Use raw/code for line slices; reductions operate on the whole file.");
    return file_view(path, kind, store);
  }
  if (name == "lt_result") {
    if(args.contains("action") || args.value("ref",std::string()).starts_with("t-") || args.value("ref",std::string()).starts_with("j-")) {
      auto out=table_tool(root,store,args).dump();store.record("",0,out.size(),"structured-result-response");return out;
    }
    auto ref = args.at("ref").get<std::string>();
    std::string out;
    if (args.contains("query")) out = store.search(ref, args.at("query").get<std::string>()).dump();
    else if (args.contains("offset")) {
      if (!args["offset"].is_number_unsigned() && (!args["offset"].is_number_integer() || args["offset"].get<long long>() < 0)) throw std::runtime_error("Invalid offset.");
      out = store.page(ref,args["offset"].get<size_t>(),number(args,"bytes",12000,16000)).dump();
    }
    else out = store.fetch(ref, number(args, "start", 1, 10000000), number(args, "count", 100, 500));
    out = bounded_text(out); store.record(ref, 0, out.size(), "retrieval-response"); return out;
  }
  throw std::runtime_error("Unknown tool.");
}
int mcp_main(const fs::path& root, bool allow_write) {
  Store store;
  while (true) {
    std::string line;
    char c;
    bool oversized = false;
    while (std::cin.get(c)) {
      if (c == '\n') break;
      if (line.size() < 1024 * 1024) line += c; else oversized = true;
    }
    if (line.empty() && !std::cin) break;
    json id = nullptr;
    try {
      if (oversized) throw std::runtime_error("MCP request exceeds 1 MiB.");
      auto request = json::parse(line);
      if (!request.is_object() || request.value("jsonrpc", std::string()) != "2.0") throw std::runtime_error("Invalid JSON-RPC request.");
      if (!request.contains("id")) continue;
      id = request["id"];
      auto method = request.at("method").get<std::string>();
      json result;
      if (method == "initialize") result = {{"protocolVersion", "2025-06-18"}, {"capabilities", {{"tools", {{"listChanged", false}}}}}, {"serverInfo", {{"name", "lesstoken"}, {"version", version}}}};
      else if (method == "ping") result = json::object();
      else if (method == "tools/list") result = {{"tools", tools(allow_write)}};
      else if (method == "tools/call") {
        try {
          auto params = request.at("params");
          if(!allow_write && params.at("name")=="lt_batch") throw ToolError("POLICY_DENIED","Batch MCP tool is disabled; user must explicitly enable file writes at installation. CLI ops can still create read-only plans.");
          auto text = mcp_call(params.at("name").get<std::string>(), params.value("arguments", json::object()), root, store, allow_write);
          (void)json(text).dump(); // Encoding failures are tool errors, not JSON-RPC parse errors.
          result = {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", false}};
        } catch (const ToolError& e) {
          result={{"content",json::array({{{"type","text"},{"text",json{{"status","error"},{"error",e.detail}}.dump()}}})},{"isError",true}};
        } catch (const std::exception& e) {
          result = {{"content", json::array({{{"type", "text"}, {"text", std::string(e.what())}}})}, {"isError", true}};
        }
      } else {
        std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Method not found"}}}}.dump() << '\n' << std::flush; continue;
      }
      std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump() << '\n' << std::flush;
    } catch (const std::exception& e) {
      std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32700}, {"message", e.what()}}}}.dump() << '\n' << std::flush;
    }
  }
  return 0;
}
}

