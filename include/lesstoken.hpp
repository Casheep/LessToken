#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace lt {
namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
inline fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
inline std::string path_text(const fs::path& path) { auto text = path.u8string(); return std::string(text.begin(), text.end()); }
inline constexpr char version[] = LESSTOKEN_VERSION;
inline constexpr size_t input_limit = 16 * 1024 * 1024;
inline constexpr size_t preview_limit = 10000;
std::string read_file(const fs::path& path, size_t limit = input_limit);
void write_file(const fs::path& path, const std::string& text);
std::vector<std::string> lines(const std::string& text);
std::string lower(std::string text);
std::string random_id();
fs::path data_root();
fs::path executable_path();
fs::path confined(const fs::path& root, const fs::path& input);
std::string quote_arg(const std::string& arg);
struct Reduction { std::string text; std::string mode = "passthrough"; size_t omitted_lines = 0; };
bool lossless_mode(const std::string& mode);
bool worthwhile_display(const std::string& raw, const Reduction& reduction, size_t extra_bytes = 0);
Reduction reduce(const std::string& input, const std::string& kind, size_t budget = 6000);
struct Store {
  fs::path root;
  explicit Store(fs::path path = data_root());
  std::string save(const std::string& raw, const json& metadata = json::object());
  std::string save_file(const fs::path& file, const json& metadata = json::object());
  std::string fetch(const std::string& id, size_t start = 1, size_t count = 100);
  json search(const std::string& id, const std::string& query, size_t context = 2, size_t limit = 10);
  json stats();
  json page(const std::string& id, size_t offset = 0, size_t bytes = 12000);
  json prune(size_t max_bytes = 512 * 1024 * 1024, size_t max_age_days = 7, const std::string& keep_ref = "");
  void record(const std::string& ref, size_t input_bytes, size_t shown_bytes, const std::string& kind);
};
std::string display(const std::string& raw, const Reduction& reduction, Store& store, const json& metadata = json::object(), bool track = true);
json byte_page(const fs::path& path, size_t offset, size_t bytes);
json search_file(const fs::path& path, const std::string& query, size_t limit = 10);
std::string file_view(const fs::path& path, const std::string& kind, Store& store, bool track = true);
void stream_file(const fs::path& path, std::ostream& output);
struct CapturedFiles { fs::path directory; ~CapturedFiles(); };
struct ProcessResult {
  std::string out, err;
  fs::path stdout_file, stderr_file;
  std::shared_ptr<CapturedFiles> files;
  uintmax_t stdout_bytes = 0, stderr_bytes = 0;
  int exit_code = 0; bool timed_out = false;
};
ProcessResult execute(const std::vector<std::string>& argv, int timeout_ms = 120000);
struct ToolError : std::runtime_error {
  json detail;
  ToolError(const std::string& code, const std::string& message, const std::string& phase = "preflight", const std::string& effect = "none");
};
json project_query(const fs::path& root, const json& args);
json table_tool(const fs::path& root, Store& store, const json& args);
json batch_tool(const fs::path& root, const json& args, bool allow_write);
json record_page(const json& rows, const json& args, size_t reserved_bytes = 0);
std::string root_identity(const fs::path& root);
std::string digest_text(const std::string& text);
std::string mcp_call(const std::string& name, const json& args, const fs::path& root, Store& store, bool allow_write = false);
int mcp_main(const fs::path& root, bool allow_write = false);
json hook(const json& event, bool explicit_opt_in = false);
json post_hook(const json& event);
json prompt_hook(const json& event);
json context_hook(const json& event);
bool restore_context_archive(const fs::path& store_root, const std::string& ref);
fs::path result_path(const fs::path& store_root, const std::string& ref);
json doctor();
int main_impl(const std::vector<std::string>& argv);
}

