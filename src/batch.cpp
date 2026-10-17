// Selected dsh-ops behaviours ported to C++ (MIT); see third_party/dsh-ops/NOTICE.md.
#include "native.hpp"
#include <algorithm>
#include <set>
#include <cstring>

namespace lt {
#ifdef _WIN32
static fs::path relative_name(const std::string& input) {
  if(input.empty() || input.size()>4096 || input.find('\0')!=std::string::npos || input.find('\\')!=std::string::npos || input.find(':')!=std::string::npos) throw ToolError("INVALID_ARGUMENT","Use nonempty relative / paths without backslashes, NUL or colons");
  auto result=from_utf8(input);
  if(result.is_absolute() || result.has_root_name()) throw ToolError("POLICY_DENIED","Batch paths must stay relative to the configured project");
  for(const auto& component:result) {
    auto part=path_text(component), folded=lower(part), stem=folded.substr(0,folded.find('.'));
    if(part.empty() || part=="." || part==".." || part.back()==' ' || part.back()=='.' || part.find_first_of("<>\"|?*")!=std::string::npos ||
      std::any_of(part.begin(),part.end(),[](unsigned char c){return c<32;}) || stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" ||
      (stem.size()==4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3]>='0' && stem[3]<='9')) throw ToolError("POLICY_DENIED","Unsafe or ambiguous batch path component");
    if(folded==".git" || folded==".lesstoken") throw ToolError("POLICY_DENIED","Batch cannot modify Git metadata or local tool state");
  }
  return result;
}
static bool overlaps(const std::string& a,const std::string& b) {
  auto fold=[](const std::string& text) {
    auto wide=from_utf8(text).native();
    auto n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr,0);
    if(n<=0) throw ToolError("INVALID_ARGUMENT","Cannot normalize batch path");
    std::wstring result(static_cast<size_t>(n),L'\0');LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,wide.data(),static_cast<int>(wide.size()),result.data(),n,nullptr,nullptr,0);return result;
  };
  auto x=fold(a),y=fold(b);return x==y || x.starts_with(y+L"/") || y.starts_with(x+L"/");
}
struct PinnedPath {
  fs::path full;
  std::vector<NativeHandle> parents;
  PinnedPath(const fs::path& root,const std::string& name) {
    auto relative=relative_name(name);auto cursor=fs::canonical(root);parents.push_back(open_directory(cursor));
    for(const auto& part:relative.parent_path()) {cursor/=part;parents.push_back(open_directory(cursor));}
    full=cursor/relative.filename();
    auto state=fs::weakly_canonical(data_root());auto candidate=full.lexically_normal().lexically_relative(state.lexically_normal());
    if(full.lexically_normal()==state || (!candidate.empty() && !candidate.is_absolute() && *candidate.begin()!=fs::path(".."))) throw ToolError("POLICY_DENIED","Batch cannot modify its own data store");
  }
};
struct Source {
  PinnedPath path;
  NativeHandle file;
  std::string content_digest;
  Source(const fs::path& root,const std::string& name,bool modifying):path(root,name),
    file(CreateFileW(path.full.c_str(),GENERIC_READ|(modifying?DELETE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)) {
    if(!file) throw ToolError("ACCESS_DENIED","Cannot open and exclusively pin source file");
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.get(),&info) || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) throw ToolError("POLICY_DENIED","This batch version accepts regular files only; directories and links are rejected");
    if(info.nFileSizeHigh || info.nFileSizeLow>512u*1024*1024) throw ToolError("OUTPUT_LIMIT","Batch source exceeds 512 MiB");
    if(info.nNumberOfLinks>1) throw ToolError("POLICY_DENIED","Hard-linked batch sources are unsupported");
  }
  std::string revision() {
    LARGE_INTEGER zero{};if(!SetFilePointerEx(file.get(),zero,nullptr,FILE_BEGIN)) throw ToolError("IO_ERROR","Cannot seek source");
    Digest digest;std::array<char,65536> block{};DWORD n=0;
    for(;;) {if(!ReadFile(file.get(),block.data(),static_cast<DWORD>(block.size()),&n,nullptr)) throw ToolError("IO_ERROR","Cannot hash source");if(!n) break;digest.add(block.data(),n);}
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file.get(),&info)) throw ToolError("IO_ERROR","Cannot read source identity");
    content_digest=digest.finish();
    auto value=content_digest+":"+std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow)+":"+
      std::to_string(info.ftLastWriteTime.dwHighDateTime)+":"+std::to_string(info.ftLastWriteTime.dwLowDateTime)+":"+std::to_string(info.dwFileAttributes);
    return digest_text(value);
  }
};
static void no_destination(const PinnedPath& path) {
  auto attrs=GetFileAttributesW(path.full.c_str());
  if(attrs!=INVALID_FILE_ATTRIBUTES) throw ToolError("REVISION_CONFLICT","Destination already exists; no overwrites");
  auto code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND && code!=ERROR_PATH_NOT_FOUND) throw ToolError("ACCESS_DENIED","Cannot inspect destination");
}
static void apply(Source& source,const std::string& op,PinnedPath* destination) {
  if(op=="copy") {
    NativeHandle out(CreateFileW(destination->full.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!out) throw ToolError("RESULT_UNKNOWN","Cannot create copy destination","execute","unknown");
    LARGE_INTEGER zero{};if(!SetFilePointerEx(source.file.get(),zero,nullptr,FILE_BEGIN)) throw ToolError("RESULT_UNKNOWN","Cannot seek source","execute","unknown");
    std::array<char,65536> bytes{};DWORD n=0,written=0;
    for(;;) {
      if(!ReadFile(source.file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)) throw ToolError("RESULT_UNKNOWN","Copy read failed; destination may be partial","execute","unknown");
      if(!n) break;
      if(!WriteFile(out.get(),bytes.data(),n,&written,nullptr) || written!=n) throw ToolError("RESULT_UNKNOWN","Copy write failed; destination may be partial","execute","unknown");
    }
    if(!FlushFileBuffers(out.get())) throw ToolError("RESULT_UNKNOWN","Cannot flush copy","execute","unknown");
    if(!SetFilePointerEx(out.get(),zero,nullptr,FILE_BEGIN)) throw ToolError("RESULT_UNKNOWN","Cannot verify copy","verify","unknown");
    Digest verified;
    for(;;) {if(!ReadFile(out.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)) throw ToolError("RESULT_UNKNOWN","Cannot read copied bytes for verification","verify","unknown");if(!n) break;verified.add(bytes.data(),n);}
    if(verified.finish()!=source.content_digest) throw ToolError("RESULT_UNKNOWN","Copied content verification failed","verify","unknown");
  } else if(op=="move") {
    auto name=destination->full.native();std::vector<unsigned char> bytes(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());info->ReplaceIfExists=FALSE;info->RootDirectory=nullptr;info->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));
    std::memcpy(info->FileName,name.data(),info->FileNameLength);
    if(!SetFileInformationByHandle(source.file.get(),FileRenameInfo,info,static_cast<DWORD>(bytes.size()))) throw ToolError("RESULT_UNKNOWN","Move failed; inspect source and destination","execute","unknown");
  } else {
    FILE_DISPOSITION_INFO info{TRUE};
    if(!SetFileInformationByHandle(source.file.get(),FileDispositionInfo,&info,sizeof(info))) throw ToolError("RESULT_UNKNOWN","Removal failed; inspect source","execute","unknown");
  }
}
static json summary(const json& job) {
  size_t done=0,failed=0,unknown=0;
  for(const auto& item:job["items"]) {auto s=item.value("state",std::string());done+=s=="completed";failed+=s=="failed";unknown+=s=="unknown";}
  auto total=job["items"].size();
  return {{"ref",job["ref"]},{"status",unknown?"unknown":failed?"failed":done==total?"completed":"planned"},{"done",done},{"failed",failed},{"unknown",unknown},{"total",total},
    {"next_action",unknown?"Inspect postconditions; unknown operations are never automatically replayed.":"Use lt_result action=page with this ref for item states."}};
}
static void validate_operations(const json& operations,const json& protect) {
  if(!operations.is_array() || operations.empty() || operations.size()>128 || !protect.is_array() || protect.size()>128) throw ToolError("INVALID_ARGUMENT","Supply 1..128 operations and at most 128 protected paths");
  for(const auto& p:protect) {(void)relative_name(p.get<std::string>());}
  std::vector<std::string> targets;
  for(const auto& operation:operations) {
    if(!operation.is_object()) throw ToolError("INVALID_ARGUMENT","Operation must be an object");
    for(auto it=operation.begin();it!=operation.end();++it) if(it.key()!="op" && it.key()!="path" && it.key()!="to") throw ToolError("INVALID_ARGUMENT","Unknown operation field");
    auto op=operation.at("op").get<std::string>();auto path=operation.at("path").get<std::string>();(void)relative_name(path);
    if(op!="copy" && op!="move" && op!="remove") throw ToolError("INVALID_ARGUMENT","op: copy, move or remove");
    std::vector<std::string> current{path};
    if(op!="remove") {auto to=operation.at("to").get<std::string>();(void)relative_name(to);current.push_back(to);} else if(operation.contains("to")) throw ToolError("INVALID_ARGUMENT","remove does not accept to");
    for(const auto& name:current) {
      for(const auto& p:protect) if(overlaps(name,p.get<std::string>())) throw ToolError("POLICY_DENIED","Operation intersects a protected path");
      for(const auto& other:targets) if(overlaps(name,other)) throw ToolError("INVALID_ARGUMENT","Batch paths must be independent and non-overlapping");
      targets.push_back(name);
    }
  }
}
#endif
json batch_tool(const fs::path& root,const json& args,bool allow_write) {
#ifndef _WIN32
  (void)root;(void)args;(void)allow_write;throw ToolError("UNSUPPORTED_PLATFORM","File batches currently support Windows only");
#else
  auto action=args.value("action",std::string());
  if(action!="plan" && action!="run" && action!="resume") throw ToolError("INVALID_ARGUMENT","Batch action: plan, run or resume");
  if(action!="plan" && !allow_write) throw ToolError("POLICY_DENIED","File writes are disabled. Start the server with --allow-write or explicitly install with -EnableFileWrites");
  auto home=record_home(root);auto lock_path=home/"batch.lock";
  NativeHandle lock(CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!lock || reparse(lock_path)) throw ToolError("BUSY","Another batch holds the project journal lock, or lock storage is unavailable");
  json job;
  if(action=="resume") {
    if(args.contains("operations") || args.contains("protect")) throw ToolError("INVALID_ARGUMENT","resume accepts only the saved ref; operations/protect cannot change");
    auto ref=args.at("ref").get<std::string>();if(!ref.starts_with("j-")) throw ToolError("INVALID_ARGUMENT","Expected a batch j- reference");
    job=load_record(root,ref);json operations=json::array();for(const auto& item:job.at("items")) operations.push_back(item.at("operation"));
    validate_operations(operations,job.at("protect"));
    for(const auto& item:job["items"]) {auto state=item.at("state").get<std::string>();if(state!="pending" && state!="unknown" && state!="completed" && state!="failed") throw ToolError("POLICY_DENIED","Invalid saved item state");}
  } else {
    if(args.contains("ref")) throw ToolError("INVALID_ARGUMENT","plan/run require operations, not a ref");
    auto operations=args.at("operations");auto protect=args.value("protect",json::array());validate_operations(operations,protect);
    job={{"version",1},{"root",path_text(fs::canonical(root))},{"root_identity",root_identity(root)},{"ref","j-"+random_id()},{"protect",protect},{"items",json::array()}};
    // Preflight every item before persisting a plan or touching business files.
    for(const auto& operation:operations) {
      Source source(root,operation["path"].get<std::string>(),false);
      if(operation["op"]!="remove") {PinnedPath destination(root,operation["to"].get<std::string>());no_destination(destination);}
      job["items"].push_back({{"operation",operation},{"revision",source.revision()},{"state","pending"},{"error",nullptr}});
    }
    save_record(root,job["ref"].get<std::string>(),job);
    if(action=="plan") return summary(job);
  }
  auto ref=job["ref"].get<std::string>();
  for(auto& item:job["items"]) {
    if(item["state"]!="pending") continue; // completed, failed and uncertain effects are never blindly repeated
    try {
      const auto& operation=item["operation"];auto op=operation["op"].get<std::string>();
      Source source(root,operation["path"].get<std::string>(),op!="copy");
      if(source.revision()!=item["revision"].get<std::string>()) throw ToolError("REVISION_CONFLICT","Source changed since planning; no operation performed");
      std::unique_ptr<PinnedPath> destination;
      if(op!="remove") {destination=std::make_unique<PinnedPath>(root,operation["to"].get<std::string>());no_destination(*destination);}
      item["state"]="unknown";save_record(root,ref,job); // durable write-ahead before the first possible effect
      apply(source,op,destination.get());
      source.file.reset();
      item["state"]="completed";item["error"]=nullptr;save_record(root,ref,job);
    } catch(const std::exception& error) {
      bool uncertain=item["state"]=="unknown" || item["state"]=="completed";
      item["state"]=uncertain?"unknown":"failed";
      if(auto structured=dynamic_cast<const ToolError*>(&error)) item["error"]=structured->detail;
      else item["error"]=ToolError(uncertain?"RESULT_UNKNOWN":"IO_ERROR",error.what(),uncertain?"execute":"preflight",uncertain?"unknown":"none").detail;
      if(uncertain) {item["error"]["effectState"]="unknown";item["error"]["phase"]="execute";item["error"]["nextAction"]="Inspect postconditions; do not blindly rerun.";}
      try {save_record(root,ref,job);} catch(...) {throw ToolError("RESULT_UNKNOWN","Cannot settle journal "+ref+"; inspect its persisted item states and filesystem postconditions","execute","unknown");}
    }
  }
  return summary(job);
#endif
}
}
