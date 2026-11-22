#include "http.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace cf {

static const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
static const char* kReferer = "https://coomerfans.com/";

Http::Http() {
  cookieJar_ = (fs::temp_directory_path() / ("coomer-cookies-" + std::to_string(getpid()) + ".txt")).string();
}

Http::~Http() {
  std::error_code ec;
  fs::remove(cookieJar_, ec);
}

void Http::cancel() {
  stop_ = true;
  pid_t p = pid_.load();
  if (p > 0) kill(p, SIGTERM);
}

void Http::sleepMs(int ms) const {
  for (int waited = 0; waited < ms; waited += 50) {
    if (stop_) throw Aborted();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - waited)));
  }
  if (stop_) throw Aborted();
}

int Http::run(const std::vector<std::string>& args, std::string& out, std::string& err) {
  if (stop_) throw Aborted();
  int po[2], pe[2];
  if (pipe(po) != 0 || pipe(pe) != 0) throw std::runtime_error("pipe() failed");

  pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork() failed");
  if (pid == 0) {
    dup2(po[1], 1);
    dup2(pe[1], 2);
    close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
    std::vector<char*> argv;
    argv.push_back((char*)"curl");
    for (auto& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    execvp("curl", argv.data());
    _exit(127);
  }
  close(po[1]);
  close(pe[1]);
  pid_ = pid;
  if (stop_) kill(pid, SIGTERM);  // cancel() raced with fork

  struct pollfd fds[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
  char buf[65536];
  int open = 2;
  while (open > 0) {
    if (poll(fds, 2, -1) < 0) break;
    for (int k = 0; k < 2; k++) {
      if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP))) continue;
      ssize_t n = read(fds[k].fd, buf, sizeof buf);
      if (n > 0) (k == 0 ? out : err).append(buf, (size_t)n);
      else { close(fds[k].fd); fds[k].fd = -1; open--; }
    }
  }
  int status = 0;
  waitpid(pid, &status, 0);
  pid_ = 0;
  if (stop_) throw Aborted();
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

std::string Http::fetchText(const std::string& url, int maxRetries, int baseDelay, int timeoutSec) {
  std::string lastError;
  for (int attempt = 0; attempt <= maxRetries; attempt++) {
    std::string out, err;
    int code = run({"-sS", "-L", "--compressed", "--connect-timeout", "20", "--max-time", std::to_string(timeoutSec),
                    "-A", kUserAgent, "-e", kReferer,
                    "-H", "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                    "-H", "Accept-Language: en-US,en;q=0.9",
                    "-b", cookieJar_, "-c", cookieJar_, "-w", "\n%{http_code}", url},
                   out, err);
    if (code == 127) throw std::runtime_error("`curl` was not found on PATH. Please install curl.");
    int http = 0;
    size_t nl = out.rfind('\n');
    if (code == 0 && nl != std::string::npos) {
      http = std::atoi(out.c_str() + nl + 1);
      out.resize(nl);
    }
    if (code == 0 && http >= 200 && http < 300) return out;
    lastError = code == 0 ? "HTTP " + std::to_string(http) : (err.empty() ? "curl exit " + std::to_string(code) : err);
    while (!lastError.empty() && (lastError.back() == '\n' || lastError.back() == '\r')) lastError.pop_back();
    if (attempt < maxRetries) sleepMs(baseDelay * (1 << attempt));
  }
  throw std::runtime_error("Failed to fetch " + url + " after " + std::to_string(maxRetries + 1) + " attempts: " + lastError);
}

Http::Downloaded Http::downloadFile(const std::string& url, const std::string& destDir, std::string filename,
                                    int maxRetries, int retryDelayMs) {
  fs::create_directories(destDir);
  if (filename.empty()) filename = "download";
  if (filename.find('.') == std::string::npos) filename += ".jpg";

  fs::path finalPath = fs::path(destDir) / filename;
  fs::path partPath = finalPath;
  partPath += ".part";

  std::error_code ec;
  if (fs::is_regular_file(finalPath, ec) && fs::file_size(finalPath, ec) > 0)
    return {finalPath.string(), (long long)fs::file_size(finalPath, ec)};

  std::string lastError;
  for (int attempt = 0; attempt <= maxRetries; attempt++) {
    fs::remove(partPath, ec);
    std::string out, err;
    int code = run({"-s", "-L", "--connect-timeout", "20", "--max-time", "180", "-A", kUserAgent, "-e", kReferer,
                    "-w", "%{http_code}", "-o", partPath.string(), url},
                   out, err);
    if (code == 127) throw std::runtime_error("`curl` was not found on PATH. Please install curl.");
    if (code != 0) {
      lastError = "curl exited with code " + std::to_string(code) + ": " + err;
    } else {
      int http = std::atoi(out.c_str());
      if (http >= 200 && http < 300) {
        fs::rename(partPath, finalPath);
        return {finalPath.string(), (long long)fs::file_size(finalPath)};
      }
      lastError = "HTTP " + std::to_string(http);
      if (http == 404) break;  // not on this host; don't burn retries
    }
    if (attempt < maxRetries) sleepMs(retryDelayMs);
  }
  fs::remove(partPath, ec);
  throw std::runtime_error("Failed to download " + url + ": " + lastError);
}

}  // namespace cf
