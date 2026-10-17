#pragma once
#include "lesstoken.hpp"
#include <array>
#include <fstream>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
namespace lt {
class NativeHandle {
  HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
  NativeHandle() = default;
  explicit NativeHandle(HANDLE h):handle_(h) {}
  NativeHandle(const NativeHandle&)=delete;
  NativeHandle& operator=(const NativeHandle&)=delete;
  NativeHandle(NativeHandle&& other) noexcept:handle_(other.handle_) {other.handle_=INVALID_HANDLE_VALUE;}
  NativeHandle& operator=(NativeHandle&& other) noexcept { if(this!=&other) {reset();handle_=other.handle_;other.handle_=INVALID_HANDLE_VALUE;} return *this; }
  ~NativeHandle(){reset();}
  void reset(){if(handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}
  HANDLE get() const {return handle_;}
  explicit operator bool() const {return handle_!=INVALID_HANDLE_VALUE;}
};
class Digest {
  BCRYPT_ALG_HANDLE algorithm_=nullptr;
  BCRYPT_HASH_HANDLE hash_=nullptr;
public:
  Digest() {
    if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw ToolError("IO_ERROR","SHA-256 unavailable");
    if(BCryptCreateHash(algorithm_,&hash_,nullptr,0,nullptr,0,0)<0) {BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw ToolError("IO_ERROR","Cannot create SHA-256 state");}
  }
  ~Digest(){if(hash_) BCryptDestroyHash(hash_);if(algorithm_) BCryptCloseAlgorithmProvider(algorithm_,0);}
  void add(const char* data,size_t size) {if(BCryptHashData(hash_,reinterpret_cast<PUCHAR>(const_cast<char*>(data)),static_cast<ULONG>(size),0)<0) throw ToolError("IO_ERROR","Cannot hash content");}
  std::string finish(){std::array<unsigned char,32> bytes{};if(BCryptFinishHash(hash_,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0) throw ToolError("IO_ERROR","Cannot finish hash");std::ostringstream out;for(auto c:bytes) out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(c);return out.str();}
};
inline bool reparse(const fs::path& path){auto a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT);}
inline NativeHandle open_directory(const fs::path& path) {
  NativeHandle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!h) throw ToolError("ACCESS_DENIED","Cannot pin directory: "+path_text(path));
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(h.get(),&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw ToolError("POLICY_DENIED","Directory links/reparse points are unsupported");
  return h;
}
}
#endif

namespace lt {
fs::path record_home(const fs::path& root);
json load_record(const fs::path& root, const std::string& ref);
void save_record(const fs::path& root, const std::string& ref, const json& value);
void atomic_json(const fs::path& path, const json& value);
void validate_ref(const std::string& ref);
}
