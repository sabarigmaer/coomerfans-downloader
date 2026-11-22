#pragma once
#include <atomic>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>

namespace cf {

struct Aborted : std::runtime_error {
  Aborted() : std::runtime_error("Aborted") {}
};

/** HTTP via the system `curl` binary (the CDN rejects non-browser TLS fingerprints). */
class Http {
 public:
  Http();
  ~Http();

  /** Request cancellation: kills any running curl and makes later calls throw Aborted. */
  void cancel();
  bool cancelled() const { return stop_.load(); }
  void sleepMs(int ms) const;  // throws Aborted when cancelled

  std::string fetchText(const std::string& url, int maxRetries = 3, int retryDelayMs = 1500, int timeoutSec = 30);

  struct Downloaded { std::string localPath; long long size = 0; };
  Downloaded downloadFile(const std::string& url, const std::string& destDir, std::string filename,
                          int maxRetries = 4, int retryDelayMs = 500);

 private:
  int run(const std::vector<std::string>& args, std::string& out, std::string& err);
  std::atomic<bool> stop_{false};
  std::atomic<pid_t> pid_{0};
  std::string cookieJar_;
};

}  // namespace cf
