#include "lesstoken.hpp"
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace lt {
namespace {
struct Change { size_t old_begin,new_begin,old_end,new_end; };
std::vector<std::string_view> source_lines(const std::string& text) {
  std::vector<std::string_view> rows;
  for(size_t at=0;at<text.size();) {
    auto end=text.find('\n',at),next=end==std::string::npos?text.size():end+1;
    rows.emplace_back(text.data()+at,next-at);at=next;
  }
  return rows;
}
std::string render_delta(const std::string& path,const std::vector<std::string_view>& old,
    const std::vector<std::string_view>& now,const std::vector<Change>& changes,size_t context) {
  std::string out="--- a/"+path+"\n+++ b/"+path+"\n";
  auto row=[&](char tag,std::string_view text) {
    out+=tag;out+=text;
    if(text.empty() || text.back()!='\n') out+="\n\\ No newline at end of file\n";
  };
  size_t hunks=0;
  for(size_t first=0;first<changes.size();) {
    size_t last=first;
    while(last+1<changes.size() && changes[last+1].old_begin-changes[last].old_end<=context*2) ++last;
    if(++hunks>16) return {}; // A fragmented patch is poor evidence; use the single hunk instead.
    const auto& begin=changes[first];const auto& end=changes[last];
    auto before=std::min({context,begin.old_begin,begin.new_begin});
    auto after=std::min({context,old.size()-end.old_end,now.size()-end.new_end});
    auto a=begin.old_begin-before,b=begin.new_begin-before;
    const auto old_end=end.old_end+after,new_end=end.new_end+after;
    out+="@@ -"+std::to_string(a+(old_end>a?1:0))+","+std::to_string(old_end-a)+
      " +"+std::to_string(b+(new_end>b?1:0))+","+std::to_string(new_end-b)+" @@\n";
    for(size_t k=first;k<=last;++k) {
      const auto& change=changes[k];
      while(a<change.old_begin) {row(' ',old[a++]);++b;}
      while(a<change.old_end) row('-',old[a++]);
      while(b<change.new_end) row('+',now[b++]);
    }
    while(a<old_end) {row(' ',old[a++]);++b;}
    first=last+1;
  }
  return out;
}
}
std::string source_delta(const std::string& path,const std::string& before,const std::string& after) {
  const auto old=source_lines(before),now=source_lines(after);
  size_t prefix=0,suffix=0;
  while(prefix<std::min(old.size(),now.size()) && old[prefix]==now[prefix]) ++prefix;
  while(suffix<std::min(old.size(),now.size())-prefix && old[old.size()-suffix-1]==now[now.size()-suffix-1]) ++suffix;
  if(prefix==old.size() && prefix==now.size()) return {};
  const auto old_end=old.size()-suffix,new_end=now.size()-suffix;
  auto fallback=render_delta(path,old,now,{{prefix,prefix,old_end,new_end}},1);
  const auto n=old_end-prefix,m=new_end-prefix;
  // Bound both work and memory, including files consisting of thousands of tiny
  // lines. Unsupported/large changes keep the original exact single-hunk path.
  constexpr size_t cell_limit=1024*1024;
  if(!n || !m || m>=cell_limit || n>=cell_limit/(m+1)) return fallback;
  std::vector<uint16_t> lengths((n+1)*(m+1));
  auto at=[&](size_t i,size_t j)->uint16_t& {return lengths[i*(m+1)+j];};
  for(size_t i=n;i-->0;) for(size_t j=m;j-->0;)
    at(i,j)=old[prefix+i]==now[prefix+j]?static_cast<uint16_t>(1+at(i+1,j+1)):std::max(at(i+1,j),at(i,j+1));
  std::vector<Change> changes;
  size_t i=0,j=0;
  while(i<n || j<m) {
    if(i<n && j<m && old[prefix+i]==now[prefix+j]) {++i;++j;continue;}
    Change change{prefix+i,prefix+j,0,0};
    do {
      if(j==m || (i<n && at(i+1,j)>=at(i,j+1))) ++i;
      else ++j;
    } while((i<n || j<m) && !(i<n && j<m && old[prefix+i]==now[prefix+j]));
    change.old_end=prefix+i;change.new_end=prefix+j;changes.push_back(change);
  }
  if(changes.size()<2) return fallback;
  auto candidate=render_delta(path,old,now,changes,3);
  // Include JSON escaping in the comparison: this patch is sent as a JSON value.
  // Small gains do not justify fragmenting the evidence into multiple hunks.
  if(candidate.empty()) return fallback;
  const auto full=json(fallback).dump().size(),compact=json(candidate).dump().size();
  if(compact>=full || full-compact<512 || full-compact<full/5+(full%5!=0)) return fallback;
  return candidate;
}
}
