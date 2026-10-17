#include "lesstoken.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lt {
std::string quote_arg(const std::string& arg) {
#ifdef _WIN32
  std::string out = "\"";
  size_t slashes = 0;
  for (char c : arg) {
    if (c == '\\') { ++slashes; continue; }
    if (c == '"') out.append(slashes * 2 + 1, '\\');
    else out.append(slashes, '\\');
    slashes = 0; out += c;
  }
  out.append(slashes * 2, '\\'); return out + '"';
#else
  std::string out = "'";
  for (char c : arg) out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
#endif
}
#ifdef _WIN32
static std::wstring wide(const std::string& value) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!n && !value.empty()) throw std::runtime_error("Invalid UTF-8 command argument.");
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
  return out;
}
#endif
ProcessResult execute(const std::vector<std::string>& argv, int timeout_ms) {
  if (argv.empty() || argv[0].empty()) throw std::runtime_error("Missing executable.");
  for (const auto& a : argv) if (a.find('\0') != std::string::npos) throw std::runtime_error("NUL in argument.");
  auto folder = fs::temp_directory_path() / ("lesstoken-process-" + random_id());
  fs::create_directory(folder);
  auto out_file = folder / "stdout", err_file = folder / "stderr";
  ProcessResult result;
  result.files = std::make_shared<CapturedFiles>(); result.files->directory = folder;
  result.stdout_file = out_file; result.stderr_file = err_file;
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE out = CreateFileW(out_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  HANDLE err = CreateFileW(err_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  auto close = [&]() { if (out != INVALID_HANDLE_VALUE) CloseHandle(out); if (err != INVALID_HANDLE_VALUE) CloseHandle(err); if (in != INVALID_HANDLE_VALUE) CloseHandle(in); };
  if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) { close(); throw std::runtime_error("Cannot create private process streams."); }
  std::string command;
  for (const auto& arg : argv) { if (!command.empty()) command += ' '; command += quote_arg(arg); }
  auto cmd = wide(command);
  STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in; si.hStdOutput = out; si.hStdError = err;
  PROCESS_INFORMATION pi{};
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) { close(); throw std::runtime_error("Cannot create process job."); }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job); close(); throw std::runtime_error("Cannot configure process job."); }
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
    auto code = GetLastError(); CloseHandle(job); close(); throw std::runtime_error("Process launch failed (Windows error " + std::to_string(code) + ").");
  }
  // Own the entire descendant tree before any project code executes.
  if (!AssignProcessToJobObject(job, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 126); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job); close();
    throw std::runtime_error("Cannot contain subprocess in job; command was not executed.");
  }
  ResumeThread(pi.hThread); CloseHandle(pi.hThread);
  auto started = std::chrono::steady_clock::now();
  while (WaitForSingleObject(pi.hProcess, 25) == WAIT_TIMEOUT) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsed > timeout_ms) { result.timed_out = true; TerminateJobObject(job, 124); break; }
  }
  WaitForSingleObject(pi.hProcess, 5000);
  DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
  result.exit_code = static_cast<int>(code); CloseHandle(pi.hProcess); CloseHandle(job); close();
#else
  pid_t pid = fork();
  if (pid == -1) throw std::runtime_error("fork failed.");
  if (!pid) {
    setpgid(0, 0);
    int o = open(out_file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600), e = open(err_file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    int i = open("/dev/null", O_RDONLY);
    if (o < 0 || e < 0 || i < 0) _exit(126);
    dup2(o, 1); dup2(e, 2); dup2(i, 0); close(o); close(e); close(i);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str())); args.push_back(nullptr);
    execvp(args[0], args.data()); _exit(127);
  }
  setpgid(pid, pid);
  auto start = std::chrono::steady_clock::now();
  int status = 0;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeout_ms) {
      kill(-pid, SIGKILL); waitpid(pid, &status, 0); result.timed_out = true; break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  kill(-pid, SIGKILL);
  result.exit_code = result.timed_out ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
  result.stdout_bytes = fs::file_size(out_file); result.stderr_bytes = fs::file_size(err_file);
  if (result.stdout_bytes <= input_limit) result.out = read_file(out_file);
  if (result.stderr_bytes <= input_limit) result.err = read_file(err_file);
  return result;
}
}

