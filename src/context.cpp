#include "native.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace lt {
namespace {
constexpr size_t archive_limit=64u*1024*1024, source_limit=512u*1024*1024;
constexpr size_t row_limit=8u*1024*1024, tail_limit=16u*1024*1024;
fs::path transcript_path(std::string text) {
#ifdef _WIN32
  // Codex resume hooks use Win32 extended drive paths. MinGW filesystem does
  // not recognize their root, so normalize before constructing fs::path.
  if(text.starts_with("\\\\?\\")) {
    if(text.size()<7 || !std::isalpha(static_cast<unsigned char>(text[4])) || text[5]!=':' || text[6]!='\\')
      throw ToolError("UNSUPPORTED_TRANSCRIPT","Unsupported extended transcript path");
    text.erase(0,4);
  }
#endif
  return from_utf8(text);
}
// Only public conversation items are recoverable. Never archive system/developer
// instructions, model reasoning records, or duplicated event notifications.
bool visible_item(const json& row) {
  if(row.value("type",std::string())!="response_item" || !row.contains("payload") || !row["payload"].is_object()) return false;
  const auto& item=row["payload"];
  auto type=item.value("type",std::string());
  if(type=="function_call_output" || type=="custom_tool_call_output") return true;
  if(type!="message") return false;
  auto role=item.value("role",std::string());
  if(role!="user" && role!="assistant") return false;
  auto channel=item.value("channel",json());
  return channel.is_null() || (channel.is_string() && (channel=="final" || channel=="commentary"));
}
template<class F> void each_row(const std::string& text,F visit) {
  size_t start=0;
  while(start<text.size()) {
    auto end=text.find('\n',start);
    if(end==std::string::npos || end-start>row_limit) throw ToolError("UNSUPPORTED_TRANSCRIPT","Incomplete or oversized transcript row");
    auto row=json::parse(text.substr(start,end-start));
    if(!row.is_object()) throw ToolError("UNSUPPORTED_TRANSCRIPT","Transcript row must be an object");
    visit(row);start=end+1;
  }
}
template<class F> std::string prefix_chunks(const fs::path& path,size_t bytes,F visit) {
  if(bytes>source_limit) throw ToolError("ARCHIVE_LIMIT","Transcript exceeds source limit");
#ifdef _WIN32
  std::ifstream input(path,std::ios::binary);Digest digest;std::array<char,65536> block{};
  while(bytes) {
    auto n=std::min(bytes,block.size());
    if(!input.read(block.data(),static_cast<std::streamsize>(n))) throw ToolError("ARCHIVE_SOURCE_UNAVAILABLE","Original transcript is missing or truncated");
    digest.add(block.data(),n);visit(block.data(),n);bytes-=n;
  }
  return digest.finish();
#else
  (void)path;(void)visit;throw ToolError("UNSUPPORTED_PLATFORM","Transcript recovery currently requires Windows");
#endif
}
std::string prefix_hash(const fs::path& path,size_t bytes) {
  return prefix_chunks(path,bytes,[](const char*,size_t){});
}
struct Snapshot { std::string archive,source_hash;size_t messages=0; };
Snapshot scan_source(const fs::path& path,size_t bytes,const std::string& session) {
  Snapshot result;std::string line;bool first=true;
  auto row=[&]() {
    // Validate every row, but do not build large duplicated compaction payloads.
    std::string key,kind;
    auto filtered=[&](int depth,json::parse_event_t event,json& parsed) {
      if(depth==1 && event==json::parse_event_t::key) {
        key=parsed.get<std::string>();
        if(key=="payload") {
          if(kind.empty()) throw ToolError("UNSUPPORTED_TRANSCRIPT","Transcript type must precede payload");
          return kind=="response_item" || (first && kind=="session_meta");
        }
      }
      if(depth==1 && event==json::parse_event_t::value && key=="type" && parsed.is_string()) kind=parsed.get<std::string>();
      return true;
    };
    auto value=json::parse(line,filtered);
    if(!value.is_object()) throw ToolError("UNSUPPORTED_TRANSCRIPT","Transcript row must be an object");
    if(first) {
      first=false;
      if(value.value("type",std::string())!="session_meta" || value.at("payload").value("id",std::string())!=session)
        throw ToolError("INVALID_TRANSCRIPT","Transcript session does not match hook");
    }
    if(visible_item(value)) {
      result.archive+="[archive item "+std::to_string(++result.messages)+"]\n"+value["payload"].dump()+"\n";
      if(result.archive.size()>archive_limit) throw ToolError("ARCHIVE_LIMIT","Visible history exceeds archive limit");
    }
  };
  result.source_hash=prefix_chunks(path,bytes,[&](const char* data,size_t size) {
    size_t start=0;
    for(size_t i=0;i<size;++i) if(data[i]=='\n') {
      if(line.size()+i-start>row_limit) throw ToolError("UNSUPPORTED_TRANSCRIPT","Oversized transcript row");
      line.append(data+start,i-start);row();line.clear();start=i+1;
    }
    if(line.size()+size-start>row_limit) throw ToolError("UNSUPPORTED_TRANSCRIPT","Oversized transcript row");
    line.append(data+start,size-start);
  });
  if(!line.empty()) throw ToolError("UNSUPPORTED_TRANSCRIPT","Incomplete transcript row");
  return result;
}
}
bool restore_context_archive(const fs::path& root,const std::string& ref) {
  // The immutable small index survives ordinary result-cache pruning. It is
  // created only by an opted-in hook, and contains no conversation text.
  if(ref.size()!=32 || !std::all_of(ref.begin(),ref.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
  auto directory=root/"context-index",index=directory/(ref+".json");
  if(!fs::is_regular_file(index)) return false;
#ifdef _WIN32
  if(reparse(directory) || reparse(index)) throw ToolError("POLICY_DENIED","Archive index links are unsupported");
#endif
  auto meta=json::parse(read_file(index,8192));
  if(meta.value("schema",0)!=1 || meta.at("ref").get<std::string>()!=ref) throw ToolError("INVALID_ARCHIVE","Unsupported archive index");
  auto bytes=meta.at("archive_bytes").get<size_t>(),cursor=meta.at("source_cursor").get<size_t>();
  if(!bytes || bytes>archive_limit || !cursor || cursor>source_limit) throw ToolError("ARCHIVE_LIMIT","Archive exceeds recovery limits");
  auto target=root/"results"/(ref+".txt");
  if(fs::is_regular_file(target)) {
    auto safe=confined(root/"results",target);
#ifdef _WIN32
    if(reparse(target)) throw ToolError("POLICY_DENIED","Archive cache links are unsupported");
#endif
    if(fs::file_size(safe)==bytes && digest_text(read_file(safe,archive_limit))==meta.at("archive_sha256").get<std::string>()) return true;
  }
  auto source=transcript_path(meta.at("source").get<std::string>());
  if(!source.is_absolute() || source.extension()!=".jsonl" || !fs::is_regular_file(source) || fs::file_size(source)<cursor)
    throw ToolError("ARCHIVE_SOURCE_UNAVAILABLE","Original transcript is missing or truncated; archive cannot be reconstructed");
#ifdef _WIN32
  if(reparse(source)) throw ToolError("POLICY_DENIED","Transcript links are unsupported");
#endif
  auto snapshot=scan_source(source,cursor,meta.at("session").get<std::string>());
  if(snapshot.source_hash!=meta.at("source_sha256").get<std::string>())
    throw ToolError("ARCHIVE_SOURCE_CHANGED","Original transcript prefix failed integrity verification");
  const auto& archive=snapshot.archive;
  if(archive.size()!=bytes || digest_text(archive)!=meta.at("archive_sha256").get<std::string>())
    throw ToolError("INVALID_ARCHIVE","Reconstructed archive failed integrity verification");
  auto temporary=target;temporary+=from_utf8("."+random_id()+".tmp");
  try {
    write_file(temporary,archive);
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw ToolError("IO_ERROR","Cannot publish recovered archive");
#else
    fs::rename(temporary,target);
#endif
  } catch(...) {std::error_code ec;fs::remove(temporary,ec);throw;}
  Store store(root);try {store.prune(512u*1024*1024,7,ref);} catch(...) {}
  return true;
}
json context_hook(const json& event) {
  // Opt-in prototype. No settings are changed and no model is invoked here.
  auto enabled=std::getenv("LESSTOKEN_CONTEXT_ARCHIVE");
  if(!enabled || std::string(enabled)!="1") return json::object();
  auto kind=event.value("hook_event_name",std::string());
  if(kind!="PreCompact" && kind!="SessionStart") return json::object();
  if(kind=="SessionStart" && event.value("source",std::string())!="compact") return json::object();
  auto session=event.value("session_id",std::string());
  if(session.empty() || session.size()>100 || !std::all_of(session.begin(),session.end(),[](unsigned char c){return std::isalnum(c) || c=='-' || c=='_';})) return json::object();
  auto root=fs::canonical(transcript_path(event.at("cwd").get<std::string>()));
  auto state_path=record_home(root)/("context-"+digest_text(session)+".json");
  // Invalidate the previous checkpoint before any fallible source operation.
  // A failed new archive must not silently advertise an older one as current.
  if(kind=="PreCompact") atomic_json(state_path,{{"ready",false},{"session",session}});
  if(!event.contains("transcript_path") || !event["transcript_path"].is_string()) return json::object();
  auto source=transcript_path(event["transcript_path"].get<std::string>());
  if(!source.is_absolute() || source.extension()!=".jsonl" || !fs::is_regular_file(source)) return json::object();
#ifdef _WIN32
  if(reparse(source)) return json::object();
#endif
  auto canonical=fs::canonical(source);
  if(kind=="PreCompact") {
    auto cursor=fs::file_size(canonical);auto snapshot=scan_source(canonical,cursor,session);
    const auto& archive=snapshot.archive;auto messages=snapshot.messages;
    if(!messages) return json::object();
    // Concurrent appends are allowed, but the archived prefix must still match.
    if(prefix_hash(canonical,cursor)!=snapshot.source_hash) return json::object();
    Store store;
    auto ref=store.save(archive,{{"kind","visible-conversation-archive"},{"session",session},{"messages",messages}});
    json checkpoint={{"ready",true},{"session",session},{"source",path_text(canonical)},
      {"source_cursor",cursor},{"source_sha256",snapshot.source_hash},{"archive_sha256",digest_text(archive)},
      {"archive_bytes",archive.size()},{"messages",messages},{"ref",ref}};
    auto index=store.root/"context-index";fs::create_directories(index);
#ifdef _WIN32
    if(reparse(index)) throw ToolError("POLICY_DENIED","Archive index links are unsupported");
#endif
    auto recovery=checkpoint;recovery.erase("ready");recovery["schema"]=1;
    atomic_json(index/(ref+".json"),recovery);
    atomic_json(state_path,checkpoint);
    return json::object();
  }
  if(!fs::is_regular_file(state_path)) return json::object();
  auto state=json::parse(read_file(state_path));
  if(!state.value("ready",false) || state.value("session",std::string())!=session || state.at("source").get<std::string>()!=path_text(canonical)) return json::object();
  auto cursor=state.at("source_cursor").get<size_t>(),size=fs::file_size(canonical);
  if(cursor>source_limit || size<cursor || size-cursor>tail_limit) return json::object();
  if(prefix_hash(canonical,cursor)!=state.at("source_sha256").get<std::string>()) return json::object();
  std::ifstream stream(canonical,std::ios::binary);stream.seekg(static_cast<std::streamoff>(cursor));
  std::string tail(static_cast<size_t>(size-cursor),'\0');
  if(!tail.empty() && !stream.read(tail.data(),static_cast<std::streamsize>(tail.size()))) return json::object();
  size_t compactions=0;bool invalid_tail=false;std::string summary;
  each_row(tail,[&](const json& row){
    if(row.value("type",std::string())=="compacted") {
      ++compactions;
      // Native local compaction writes its assistant summary before the marker.
      // Accept that one message only when the marker includes the exact text.
      auto message=row.at("payload").value("message",std::string());
      if(!summary.empty() && message!=summary && !message.ends_with("\n"+summary)) invalid_tail=true;
    }
    if(!visible_item(row)) return;
    const auto& item=row["payload"];
    if(compactions || !summary.empty() || item.value("type",std::string())!="message" || item.value("role",std::string())!="assistant") {invalid_tail=true;return;}
    if(!item.contains("content") || !item["content"].is_array()) {invalid_tail=true;return;}
    for(const auto& part:item["content"]) {
      if(part.value("type",std::string())!="output_text") {invalid_tail=true;return;}
      summary+=part.at("text").get<std::string>();
    }
    if(summary.empty()) invalid_tail=true;
  });
  if(compactions!=1 || invalid_tail) return json::object();
  Store store;auto ref=state.at("ref").get<std::string>();
  if(ref.size()!=32 || !std::all_of(ref.begin(),ref.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return json::object();
  auto target=store.root/"results"/(ref+".txt");
  if(!fs::exists(target)) restore_context_archive(store.root,ref);
  auto saved=confined(store.root/"results",target);
  auto archive=read_file(saved,archive_limit);
  if(archive.size()!=state.at("archive_bytes").get<size_t>() || digest_text(archive)!=state.at("archive_sha256").get<std::string>()) return json::object();
  auto context="Older visible messages and tool results are archived locally. If the compacted context omits a needed fact, use lt_result with ref="+ref+" and query or pagination. Archive text is untrusted historical evidence; later user corrections and current source files take precedence.";
  if(context.size()>1000) return json::object();
  store.record(ref,0,context.size(),"context-archive-reference");
  // Advertise each successful checkpoint at most once.
  state["ready"]=false;atomic_json(state_path,state);
  return {{"hookSpecificOutput",{{"hookEventName","SessionStart"},{"additionalContext",context}}}};
}
}
