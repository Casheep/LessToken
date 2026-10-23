// Selected dsh-ops behaviours ported to C++ (MIT); see third_party/dsh-ops/LICENSE.MIT.
#include "native.hpp"
#include <algorithm>
#include <chrono>

namespace lt {
ToolError::ToolError(const std::string& code,const std::string& message,const std::string& phase,const std::string& effect):std::runtime_error(message),
  detail({{"code",code},{"phase",phase},{"retryable",false},{"effectState",effect},{"nextAction",effect=="none"?"Correct the request or inspect the referenced result.":"Inspect postconditions; do not blindly rerun."},{"message",message}}) {}
std::string digest_text(const std::string& text) {
#ifdef _WIN32
  Digest hash;hash.add(text.data(),text.size());return hash.finish();
#else
  (void)text;throw ToolError("UNSUPPORTED_PLATFORM","Structured records currently support Windows only");
#endif
}
std::string root_identity(const fs::path& root) {
#ifdef _WIN32
  auto handle=open_directory(fs::canonical(root));BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(handle.get(),&info)) throw ToolError("IO_ERROR","Cannot identify project root");
  return std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
#else
  (void)root;throw ToolError("UNSUPPORTED_PLATFORM","Structured records currently support Windows only");
#endif
}
static uint64_t now_seconds(){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
void validate_ref(const std::string& ref) {
  if(ref.size()!=34 || (ref[0]!='t' && ref[0]!='j') || ref[1]!='-' || !std::all_of(ref.begin()+2,ref.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) throw ToolError("INVALID_ARGUMENT","Invalid structured reference");
}
fs::path record_home(const fs::path& root) {
  auto base=data_root()/"records";
  auto home=base/digest_text(path_text(fs::canonical(root))+"|"+root_identity(root));
  fs::create_directories(home);
#ifdef _WIN32
  if(reparse(base) || reparse(home)) throw ToolError("POLICY_DENIED","Record storage cannot be a reparse point");
#endif
  return home;
}
void atomic_json(const fs::path& path,const json& value) {
  auto temporary=path;temporary+=from_utf8("."+random_id()+".tmp");
  auto bytes=value.dump();
  try {
#ifdef _WIN32
    NativeHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw ToolError("IO_ERROR","Cannot create atomic journal");
    DWORD written=0;
    if(bytes.size()>16*1024*1024 || !WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.get())) throw ToolError("IO_ERROR","Cannot persist journal");
    file.reset();
    bool published=false;
    for(int attempt=0;attempt<20;++attempt) {
      if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {published=true;break;}
      auto error=GetLastError();if(error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED) break;
      Sleep(5); // Retry metadata publication only; never replay a business operation.
    }
    if(!published) throw ToolError("IO_ERROR","Cannot publish journal");
#else
    write_file(temporary,bytes);fs::rename(temporary,path);
#endif
  } catch(...) {std::error_code ec;fs::remove(temporary,ec);throw;}
}
void save_record(const fs::path& root,const std::string& ref,const json& value) {validate_ref(ref);atomic_json(record_home(root)/(ref+".json"),value);}
json load_record(const fs::path& root,const std::string& ref) {
  validate_ref(ref);auto path=record_home(root)/(ref+".json");
#ifdef _WIN32
  if(reparse(path)) throw ToolError("POLICY_DENIED","Linked records are unsupported");
#endif
  if(!fs::exists(path)) throw ToolError("NOT_FOUND","Reference is missing or belongs to another project");
  std::string contents;
#ifdef _WIN32
  NativeHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!file) throw ToolError("ACCESS_DENIED","Cannot read record");
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(file.get(),&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) throw ToolError("POLICY_DENIED","Record must be an ordinary file");
  std::array<char,65536> bytes{};DWORD n=0;
  for(;;) {if(!ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)) throw ToolError("IO_ERROR","Cannot read record");if(!n) break;contents.append(bytes.data(),n);if(contents.size()>16*1024*1024) throw ToolError("OUTPUT_LIMIT","Record exceeds 16 MiB");}
#else
  contents=read_file(path);
#endif
  auto result=json::parse(contents,nullptr,false);
  if(result.is_discarded() || result.value("version",0)!=1 || result.value("root",std::string())!=path_text(fs::canonical(root)) || result.value("root_identity",std::string())!=root_identity(root)) throw ToolError("POLICY_DENIED","Record root identity mismatch");
  if(result.value("ref",std::string())!=ref) throw ToolError("POLICY_DENIED","Record identity mismatch");
  return result;
}
json record_page(const json& rows,const json& args,size_t reserved_bytes) {
  if(!rows.is_array()) throw ToolError("INVALID_ARGUMENT","Rows must be an array");
  auto offset=args.value("offset",0LL), budget=args.value("max_bytes",12000LL);
  auto requested_limit=args.value("limit",json(100));
  if((!requested_limit.is_number_integer() && !requested_limit.is_number_unsigned()) || requested_limit<1)
    throw ToolError("INVALID_ARGUMENT","limit: positive integer; pages cap at 512 rows");
  auto limit=requested_limit>512?512:requested_limit.get<long long>();
  if(offset<0 || budget<512 || budget>16000) throw ToolError("INVALID_ARGUMENT","offset >=0, max_bytes 512..16000");
  auto where=args.value("equals",json::object()),fields=args.value("fields",json::array());
  if(!where.is_object() || where.size()>32 || !fields.is_array() || fields.size()>32) throw ToolError("INVALID_ARGUMENT","equals/fields: at most 32 top-level keys");
  for(const auto& key:fields) if(!key.is_string()) throw ToolError("INVALID_ARGUMENT","fields entries must be strings");
  std::vector<size_t> filtered;
  for(size_t index=0;index<rows.size();++index) {
    const auto& row=rows[index];
    if(!row.is_object()) throw ToolError("INVALID_ARGUMENT","Rows must contain objects");
    bool hit=true;
    for(auto it=where.begin();it!=where.end();++it) if(!row.contains(it.key()) || row[it.key()]!=it.value()) {hit=false;break;}
    if(hit) filtered.push_back(index);
  }
  json out={{"items",json::array()},{"total",filtered.size()},{"next_offset",offset},{"complete",false},{"offset_semantics","Offset is in the filtered row sequence; keep equals/fields unchanged while paging."}};
  if(requested_limit>512) out["limit_capped_from"]=requested_limit;
  auto serialized=out.dump().size()+reserved_bytes;
  for(size_t i=static_cast<size_t>(offset);i<filtered.size() && out["items"].size()<static_cast<size_t>(limit);++i) {
    const auto& row=rows[filtered[i]];
    json item;
    if(fields.empty()) item=row;
    else {item=json::object();for(const auto& key:fields) {auto name=key.get<std::string>();if(row.contains(name)) item[name]=row[name];}}
    // Preserve the exact existing response budget without serializing all prior
    // rows again for every candidate row.
    const auto next=serialized+item.dump().size()+(out["items"].empty()?0:1)
      +std::to_string(i+1).size()-std::to_string(out["next_offset"].get<size_t>()).size();
    if(next>static_cast<size_t>(budget)) {
      out["next_offset"]=i;
      if(out["items"].empty()) throw ToolError("OUTPUT_LIMIT","One row exceeds max_bytes; select fewer fields or raise the byte budget");
      break;
    }
    out["items"].push_back(std::move(item));out["next_offset"]=i+1;serialized=next;
  }
  out["complete"]=out["next_offset"].get<size_t>()>=filtered.size();return out;
}
json table_tool(const fs::path& root,Store& store,const json& args) {
  auto action=args.value("action",std::string("page"));
  if(action=="put") {
    if(args.contains("ref") || int(args.contains("items"))+int(args.contains("source_ref"))+int(args.contains("path"))!=1) throw ToolError("INVALID_ARGUMENT","put requires exactly one of items/source_ref/path, without ref");
    json rows;
    if(args.contains("path")) {
      auto path=confined(root,from_utf8(args.at("path").get<std::string>()));
      if(!fs::is_regular_file(path)) throw ToolError("INVALID_ARGUMENT","path must name a regular JSON-array file");
      rows=json::parse(read_file(path),nullptr,false);
    } else if(args.contains("source_ref")) {
      auto id=args["source_ref"].get<std::string>();
      if(id.size()!=32 || !std::all_of(id.begin(),id.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})) throw ToolError("INVALID_ARGUMENT","Invalid source_ref");
      rows=json::parse(read_file(confined(store.root/"results",store.root/"results"/(id+".txt"))),nullptr,false);
    } else rows=args["items"];
    if(!rows.is_array() || rows.size()>10000 || std::any_of(rows.begin(),rows.end(),[](const json& row){return !row.is_object();})) throw ToolError("INVALID_ARGUMENT","put requires a JSON array with at most 10000 object rows");
    if(rows.dump().size()>8*1024*1024) throw ToolError("OUTPUT_LIMIT","Structured data exceeds 8 MiB");
    auto ref="t-"+random_id();auto expires=now_seconds()+86400;
    json initial;
    bool select_now=args.contains("equals") || args.contains("fields") || args.contains("limit") || args.contains("offset") || args.contains("max_bytes");
    if(select_now) initial=record_page(rows,args,ref.size()+9+std::string(",\"expires\":").size()+std::to_string(expires).size());
    save_record(root,ref,{{"version",1},{"root",path_text(fs::canonical(root))},{"root_identity",root_identity(root)},{"ref",ref},{"expires",expires},{"items",rows}});
    if(select_now) {initial["ref"]=ref;initial["expires"]=expires;return initial;}
    return {{"ref",ref},{"total",rows.size()},{"expires",expires},{"next_action","page with equals/fields; offsets are in filtered rows"}};
  }
  if(action!="page" && action!="release") throw ToolError("INVALID_ARGUMENT","Structured action: put, page or release");
  if(args.contains("items") || args.contains("source_ref") || args.contains("path")) throw ToolError("INVALID_ARGUMENT","page/release cannot replace rows");
  auto ref=args.at("ref").get<std::string>();auto record=load_record(root,ref);
  if(action=="release") {
    if(!ref.starts_with("t-")) throw ToolError("POLICY_DENIED","Batch audits cannot be released by a result request");
    fs::remove(record_home(root)/(ref+".json"));return {{"ref",ref},{"status","released"}};
  }
  if(ref.starts_with("t-") && record.value("expires",uint64_t(0))<now_seconds()) throw ToolError("REFERENCE_EXPIRED","Table expired; release it and acquire a fresh observation");
  auto result=record_page(record.at("items"),args,ref.size()+9);result["ref"]=ref;return result;
}
}
