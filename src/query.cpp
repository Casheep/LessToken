// Selected dsh-ops behaviours ported to C++ (MIT); see third_party/dsh-ops/LICENSE.MIT.
#include "lesstoken.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace lt {
static std::string relative_display(const fs::path& path,const fs::path& root) {auto value=path_text(fs::relative(path,root));std::replace(value.begin(),value.end(),'\\','/');return value;}
static bool excluded(const fs::path& p) {
  auto n = lower(path_text(p.filename()));
  return n.empty() || n[0] == '.' || n == "node_modules" || n == "build" || n == "dist" || n == "vendor" || n == "third_party" || n == "target";
}
static bool glob_match(const std::string& pattern, const std::string& text) {
  std::vector<unsigned char> prev(text.size()+1), next(text.size()+1); prev[0] = 1;
  for (size_t i=0; i<pattern.size(); ++i) {
    std::fill(next.begin(), next.end(), 0);
    if (pattern[i]=='*') {
      bool recursive = i+1<pattern.size() && pattern[i+1]=='*'; if(recursive) ++i;
      next[0]=prev[0];
      for(size_t j=1;j<=text.size();++j) next[j]=prev[j] || (next[j-1] && (recursive || text[j-1]!='/'));
    } else for(size_t j=1;j<=text.size();++j) next[j]=prev[j-1] && (pattern[i]==text[j-1] || (pattern[i]=='?' && text[j-1]!='/'));
    prev.swap(next);
  }
  return prev.back();
}
static bool selected(const std::string& path, const json& patterns) {
  bool positive=false, included=false;
  for(const auto& entry:patterns) {
    if(!entry.is_string()) throw ToolError("INVALID_ARGUMENT", "glob entries must be strings");
    auto p=entry.get<std::string>();
    if(p.empty() || p.size()>256) throw ToolError("INVALID_ARGUMENT", "glob entries use 1..256 bytes");
    bool negative=p[0]=='!'; if(negative) p.erase(0,1); else positive=true;
    bool hit=glob_match(p,path) || (p.starts_with("**/") && glob_match(p.substr(3),path));
    if(negative && hit) return false;
    if(!negative && hit) included=true;
  }
  return !positive || included;
}
// Merge only source-line windows whose shared text is identical (or a prefix
// clipped by the existing context bound). Distinct files and gaps stay separate.
static void merge_contexts(json& items,size_t first) {
  auto decode=[](const std::string& text,std::map<size_t,std::string>& out) {
    std::istringstream stream(text);std::string row;
    while(std::getline(stream,row)) {
      auto split=row.find(": ");
      if(split==std::string::npos || split==0 || !std::all_of(row.begin(),row.begin()+static_cast<long long>(split),[](char c){return c>='0' && c<='9';})) return false;
      out.emplace(static_cast<size_t>(std::stoull(row.substr(0,split))),row.substr(split+2));
    }
    return !out.empty();
  };
  size_t index=first;
  while(index+1<items.size()) {
    const auto& left=items[index];const auto& right=items[index+1];
    std::map<size_t,std::string> a,b;
    if(left["path"]!=right["path"] || left.value("clipped",false) || right.value("clipped",false)
        || !decode(left["context"],a) || !decode(right["context"],b)
        || b.begin()->first>a.rbegin()->first) {++index;continue;}
    bool compatible=true;
    for(const auto& [line,text]:b) {
      auto found=a.find(line);
      if(found==a.end()) a.emplace(line,text);
      else if(text.starts_with(found->second)) found->second=text;
      else if(!found->second.starts_with(text)) {compatible=false;break;}
    }
    if(!compatible) {++index;continue;}
    std::string context;
    for(const auto& [line,text]:a) {if(!context.empty()) context+='\n';context+=std::to_string(line)+": "+text;}
    auto merged=left;merged["context"]=context;
    auto lines=left.value("lines",json::array({left["line"]}));
    for(const auto& line:right.value("lines",json::array({right["line"]}))) lines.push_back(line);
    merged["lines"]=lines;
    if(right.contains("matched_queries")) {
      auto queries=left.at("matched_queries");
      for(const auto& query:right.at("matched_queries"))
        if(std::find(queries.begin(),queries.end(),query)==queries.end()) queries.push_back(query);
      merged["matched_queries"]=std::move(queries);
    }
    if(context.size()>12000 || merged.dump().size()>=left.dump().size()+right.dump().size()) {++index;continue;}
    items[index]=std::move(merged);items.erase(index+1);
  }
}
json project_query(const fs::path& root, const json& args) {
  auto mode=args.value("mode", std::string("content"));
  if(mode!="content" && mode!="files" && mode!="count" && mode!="summary") throw ToolError("INVALID_ARGUMENT", "mode: content, files, count or summary");
  const bool multi=args.contains("queries");
  if(multi==args.contains("query")) throw ToolError("INVALID_ARGUMENT","Use exactly one of query or queries[1..8]");
  auto requested=multi?args.at("queries"):json::array({args.at("query")});
  if(!requested.is_array() || requested.empty() || requested.size()>8) throw ToolError("INVALID_ARGUMENT","queries requires 1..8 literal strings");
  std::vector<std::string> queries,needles;size_t longest=0;
  for(const auto& value:requested) {
    if(!value.is_string()) throw ToolError("INVALID_ARGUMENT","Queries must be literal strings");
    auto query=value.get<std::string>();
    if(query.empty() || query.size()>1024 || query.find('\0')!=std::string::npos) throw ToolError("INVALID_ARGUMENT","Use literal queries of 1..1024 bytes");
    longest=std::max(longest,query.size());queries.push_back(query);needles.push_back(lower(query));
  }
  if(multi && requested.dump().size()>8192) throw ToolError("INVALID_ARGUMENT","Combined serialized queries must fit within 8192 bytes");
  std::vector<size_t> query_lines(queries.size()),query_files(queries.size());
  auto requested_limit=args.value("limit",json(10));
  if((!requested_limit.is_number_integer() && !requested_limit.is_number_unsigned()) || requested_limit<1)
    throw ToolError("INVALID_ARGUMENT", "limit: positive integer; previews cap at 100 hits");
  auto limit=requested_limit>100?100:requested_limit.get<int>();
  auto window=[&](const char* key,int fallback) {
    if(!args.contains(key)) return fallback;
    const auto& value=args[key];
    if((!value.is_number_integer() && !value.is_number_unsigned()) || value<0)
      throw ToolError("INVALID_ARGUMENT", "before/after: nonnegative integers; previews cap at 40 lines");
    return value>40?40:value.get<int>();
  };
  const auto before=window("before",1), after=window("after",2);
  if(mode!="content" && (args.contains("before") || args.contains("after")))
    throw ToolError("INVALID_ARGUMENT", "before/after require mode=content");
  auto patterns=args.value("glob", json::array());
  if(!patterns.is_array() || patterns.size()>32) throw ToolError("INVALID_ARGUMENT", "glob: at most 32 patterns");
  (void)selected("",patterns);
  auto target=confined(root,from_utf8(args.value("path",std::string("."))));
  bool single=fs::is_regular_file(target), walk_complete=true;
  std::vector<fs::path> files;
  if(single) {if(selected(relative_display(target,target.parent_path()),patterns)) files.push_back(target);}
  else {
    std::error_code ec;
    auto it=fs::recursive_directory_iterator(target,fs::directory_options::none,ec);
    if(ec) throw ToolError("ACCESS_DENIED","Cannot enumerate selected root");
    for(;it!=fs::recursive_directory_iterator();it.increment(ec)) {
      if(ec) { walk_complete=false; ec.clear(); break; }
      bool link=fs::is_symlink(it->symlink_status());
#ifdef _WIN32
      auto attrs=GetFileAttributesW(it->path().c_str());
      link=link || (attrs!=INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT));
#endif
      if(it.depth()>12 || link || excluded(it->path()) || fs::absolute(it->path()).lexically_normal()==fs::absolute(data_root()).lexically_normal()) {if(it.depth()>12) walk_complete=false; if(it->is_directory()) it.disable_recursion_pending(); continue; }
      if(it->is_regular_file() && selected(relative_display(it->path(),target),patterns)) files.push_back(it->path());
      if(files.size()>=1000) {walk_complete=false; break;}
    }
    if(ec) walk_complete=false;
  }
  std::sort(files.begin(),files.end());
  json items=json::array(); size_t scanned=0, scanned_files=0, matching_files=0, matching_lines=0, skipped_files=0, content_hits=0;
  bool scan_complete=walk_complete, result_complete=true;
  const size_t budget=single?512u*1024*1024:16u*1024*1024;
  auto add=[&](const json& item) {
    // Content windows are bounded by 100 hits and 12 KiB per window. Apply the
    // final response byte budget AFTER merging their overlapping source lines;
    // budgeting duplicate windows here can hide hits that would all fit merged.
    if((mode=="content"?content_hits:items.size())<static_cast<size_t>(limit) &&
      (mode=="content" || items.dump(-1,' ',false,json::error_handler_t::replace).size()+item.dump(-1,' ',false,json::error_handler_t::replace).size()<15500)) {items.push_back(item);if(mode=="content") ++content_hits;}
    else result_complete=false;
  };
  for(const auto& path:files) {
    if(scanned>=budget) {scan_complete=false;break;}
    try {
      auto safe=confined(root,path);
      auto prior_items=items.size();
      auto prior_content_hits=content_hits;
      auto bytes=fs::file_size(safe);
      if(!single && bytes>1024*1024) {++skipped_files;scan_complete=false;continue;}
      std::ifstream file(safe,std::ios::binary); if(!file) throw ToolError("ACCESS_DENIED","Cannot read selected file");
      std::array<char,65536> block{}; std::string overlap, line_prefix; size_t line=1, hits=0; bool matched=false, binary=false;
      std::vector<bool> line_matches(queries.size());std::vector<size_t> file_query_lines(queries.size());
      constexpr size_t context_budget=12000;
      std::deque<std::pair<std::string,bool>> previous;
      std::vector<std::pair<size_t,int>> pending_context;
      bool line_clipped=false;
      auto finish=[&]() {
        for(auto& pending:pending_context) if(pending.second>0) {
          auto context=items[pending.first]["context"].get<std::string>();
          auto extra="\n"+std::to_string(line)+": "+line_prefix;
          // Never cut a source line silently or emit broken UTF-8.
          if(context.size()+extra.size()<=context_budget) context+=extra;
          else items[pending.first]["clipped"]=true;
          if(line_clipped) items[pending.first]["clipped"]=true;
          items[pending.first]["context"]=context;--pending.second;
        }
        std::erase_if(pending_context,[](const auto& pending){return pending.second<=0;});
        if(matched) {
          ++hits;
          for(size_t q=0;q<queries.size();++q) if(line_matches[q]) ++file_query_lines[q];
          if(mode=="content") {
            std::string context=std::to_string(line)+": "+line_prefix;
            bool clipped=line_clipped;
            for(auto it=previous.rbegin();it!=previous.rend();++it) {
              if(context.size()+it->first.size()+1>context_budget) {clipped=true;break;}
              context=it->first+"\n"+context;clipped=clipped || it->second;
            }
            json item={{"path",relative_display(safe,root)},{"line",line},{"context",context}};
            if(multi) {
              item["matched_queries"]=json::array();
              for(size_t q=0;q<queries.size();++q) if(line_matches[q]) item["matched_queries"].push_back(queries[q]);
            }
            if(clipped) item["clipped"]=true;
            auto prior=items.size();
            add(item);
            if(items.size()>prior && after) pending_context.emplace_back(prior,after);
          }
        }
        if(mode=="content" && before) {
          previous.emplace_back(std::to_string(line)+": "+line_prefix,line_clipped);
          if(previous.size()>static_cast<size_t>(before)) previous.pop_front();
        }
        ++line; matched=false;line_clipped=false;line_prefix.clear();overlap.clear();
        std::fill(line_matches.begin(),line_matches.end(),false);
      };
      while(file && scanned<budget) {
        file.read(block.data(),static_cast<std::streamsize>(std::min<size_t>(block.size(),budget-scanned)));
        auto n=static_cast<size_t>(file.gcount()); if(!n) break; scanned+=n;
        if(std::find(block.begin(),block.begin()+static_cast<long long>(n),'\0')!=block.begin()+static_cast<long long>(n)) {binary=true;break;}
        size_t begin=0;
        while(begin<n) {
          size_t end=begin; while(end<n && block[end]!='\n') ++end;
          std::string fragment(block.data()+begin,end-begin);
          if(mode=="content") {
            if(line_prefix.size()+fragment.size()>1800) line_clipped=true;
            if(line_prefix.size()<1800) line_prefix+=fragment.substr(0,1800-line_prefix.size());
          }
          auto combined=overlap+fragment;
          auto folded=lower(combined);auto position=std::string::npos;
          for(size_t q=0;q<needles.size();++q) if(!line_matches[q]) {
            auto found=folded.find(needles[q]);
            if(found!=std::string::npos) {line_matches[q]=true;position=std::min(position,found);}
          }
          if(!matched && position!=std::string::npos) {
            matched=true;
            if(mode=="content") {
              auto prefix=lower(line_prefix);bool visible=false;
              for(size_t q=0;q<needles.size();++q) if(line_matches[q] && prefix.find(needles[q])!=std::string::npos) {visible=true;break;}
              if(!visible) {line_prefix=combined.substr(position>200?position-200:0,1800);line_clipped=true;}
            }
          }
          overlap=combined.substr(combined.size()>longest?combined.size()-longest:0);
          if(end<n) finish();
          begin=end+1;
        }
      }
      if((matched || !line_prefix.empty() || !overlap.empty()) && !binary) finish();
      ++scanned_files;
      if(binary) {while(items.size()>prior_items) items.erase(items.size()-1);content_hits=prior_content_hits;++skipped_files;scan_complete=false;continue;}
      if(mode=="content") merge_contexts(items,prior_items);
      if(file.bad()) {scan_complete=false;++skipped_files;}
      if(scanned>=budget && file.peek()!=std::char_traits<char>::eof()) scan_complete=false;
      for(size_t q=0;q<queries.size();++q) {query_lines[q]+=file_query_lines[q];query_files[q]+=file_query_lines[q]>0;}
      if(hits) {
        ++matching_files; matching_lines+=hits;
        if(mode=="files" || mode=="count") {
          json item={{"path",relative_display(safe,root)}};
          if(mode=="count") item["count"]=hits;
          add(item);
        }
      }
    } catch(const std::exception&) {++skipped_files;scan_complete=false;}
  }
  json result={{"mode",mode},{"matching_files",matching_files},{"matching_lines",matching_lines},{"files_scanned",scanned_files},{"scanned_bytes",scanned},
    {"skipped_files",skipped_files},{"scan_complete",scan_complete},{"complete",scan_complete && result_complete},
    {"note","Case-insensitive literal search; counts are matching lines. Scope excludes hidden/generated/skipped files."}};
  if(mode=="content") {result["before"]=before;result["after"]=after;}
  if(multi) {
    result["query_counts"]=json::array();
    for(size_t q=0;q<queries.size();++q) result["query_counts"].push_back({{"query",queries[q]},{"matching_lines",query_lines[q]},{"matching_files",query_files[q]}});
    result["note"]=result["note"].get<std::string>()+" Multi-query totals count the union of matching lines; query_counts are independent.";
  }
  if(requested_limit>100) result["limit_capped_from"]=requested_limit;
  for(const auto& key:{"before","after"}) if(args.contains(key) && args[key]>40) {
    result[std::string(key)+"_capped_from"]=args[key];result["context_complete"]=false;
  }
  if(mode!="summary") result["matches"]=items;
  auto safe=json::parse(result.dump(-1,' ',false,json::error_handler_t::replace));
  while(safe.dump().size()>17500 && safe.contains("matches") && !safe["matches"].empty()) {safe["matches"].erase(safe["matches"].size()-1);safe["complete"]=false;}
  return safe;
}
}
