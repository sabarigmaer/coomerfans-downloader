#pragma once
#include <functional>
#include <string>

#include "http.hpp"
#include "types.hpp"

namespace cf {

struct Stats {
  int pages = 0, posts = 0, files = 0;
  long long bytes = 0;
};

struct Callbacks {
  std::function<void(const std::string&)> log;
  std::function<void(const Stats&)> stats;
  /** label = what is happening, count = "Page 3 · post 2/20", frac = 0..1 */
  std::function<void(const std::string& label, const std::string& count, float frac)> current;
};

struct Options {
  std::string outputDir = "./downloads";
  int delayMs = 500;
};

class Downloader {
 public:
  Downloader(Options opts, Callbacks cb) : opts_(std::move(opts)), cb_(std::move(cb)) {}

  /** Thread-safe; the running job saves its progress and returns. */
  void stop() { http_.cancel(); }
  bool stopped() const { return http_.cancelled(); }

  void downloadFileList(const std::string& startUrl);
  void downloadContent(const std::string& startUrl);

  std::string creatorDir() const { return creatorDir_; }

 private:
  Http::Downloaded downloadRobust(const std::string& fileUrl, const std::string& postUrl, size_t fIdx,
                                  const std::string& destDir, const std::string& filename);
  void log(const std::string& m) { if (cb_.log) cb_.log(m); }

  Options opts_;
  Callbacks cb_;
  Http http_;
  std::string creatorDir_;
};

}  // namespace cf
