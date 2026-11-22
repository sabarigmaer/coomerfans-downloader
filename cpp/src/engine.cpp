#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "scraper.hpp"
#include "text.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace cf {

namespace {

std::string isoNow() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}

std::string formatBytes(long long b) {
  if (b <= 0) return "0 B";
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double v = (double)b;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  char buf[32];
  std::snprintf(buf, sizeof buf, i ? "%.2f %s" : "%.0f %s", v, u[i]);
  return buf;
}

std::string fileExtension(const std::string& url, const std::string& def = ".jpg") {
  std::string origin, path;
  splitUrl(url, origin, path);
  size_t slash = path.rfind('/');
  size_t dot = path.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    std::string ext = toLower(path.substr(dot));
    if (ext.size() >= 2 && ext.size() <= 5) return ext;
  }
  return def;
}

// img<N>.coomerfans.com -> the other nine mirrors
std::vector<std::string> subdomainVariants(const std::string& url) {
  static const std::regex re(R"(^(https?://)img(\d+)(\.coomerfans\.com)(.*)$)", std::regex::icase);
  std::vector<std::string> out;
  std::smatch m;
  if (std::regex_match(url, m, re)) {
    int orig = std::atoi(m[2].str().c_str());
    for (int n = 1; n <= 10; n++)
      if (n != orig) out.push_back(m[1].str() + "img" + std::to_string(n) + m[3].str() + m[4].str());
  }
  return out;
}

std::string hostOf(const std::string& url) {
  std::string origin, path;
  splitUrl(url, origin, path);
  size_t s = origin.find("://");
  return s == std::string::npos ? url : origin.substr(s + 3);
}

void writeFileAtomic(const fs::path& p, const std::string& content) {
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << content;
  }
  fs::rename(tmp, p);
}

std::string dumpJson(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

struct Storage {
  fs::path creatorDir, filesDir, progressPath, listPath;
  Storage(const std::string& base, const std::string& creator) {
    creatorDir = fs::path(base) / creator;
    filesDir = creatorDir / "files";
    progressPath = creatorDir / "progress.json";
    listPath = creatorDir / "file_list.json";
  }
  void init() const { fs::create_directories(filesDir); }

  bool loadProgress(ProgressState& out) const {
    try {
      std::ifstream f(progressPath);
      if (!f) return false;
      out = json::parse(f).get<ProgressState>();
      return true;
    } catch (...) { return false; }
  }

  void saveProgress(ProgressState& s) const {
    init();
    s.lastUpdated = isoNow();
    writeFileAtomic(progressPath, dumpJson(json(s)));
  }

  bool loadExistingList(std::vector<PageData>& pages) const {
    try {
      std::ifstream f(listPath);
      if (!f) return false;
      json j = json::parse(f);
      pages = j.at("pages").get<std::vector<PageData>>();
      return !pages.empty();
    } catch (...) { return false; }
  }

  void writeList(const CreatorInfo& info, const std::map<std::string, PageData>& pageMap) const {
    std::vector<const PageData*> v;
    for (auto& [k, p] : pageMap) v.push_back(&p);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->pageNumber < b->pageNumber; });
    writeList(info, v);
  }

  void writeList(const CreatorInfo& info, const std::vector<const PageData*>& pages) const {
    init();
    int totalPosts = 0;
    json arr = json::array();
    for (auto* p : pages) {
      totalPosts += (int)p->posts.size();
      json posts = json::array();
      for (auto& post : p->posts) {
        json pj = {{"id", post.id}, {"title", post.title}, {"safeTitle", post.safeTitle}, {"url", post.url},
                   {"date", post.date}, {"status", post.status}};
        if (!post.files.empty()) pj["files"] = post.files;
        posts.push_back(std::move(pj));
      }
      arr.push_back({{"pageNumber", p->pageNumber}, {"pageUrl", p->pageUrl}, {"postCount", p->posts.size()}, {"posts", posts}});
    }
    json out = {{"creator", info.username}, {"service", info.service}, {"sourceUrl", info.normalizedUrl},
                {"totalPages", pages.size()}, {"totalPosts", totalPosts}, {"generatedAt", isoNow()}, {"pages", arr}};
    writeFileAtomic(listPath, dumpJson(out));
  }
};

}  // namespace

Http::Downloaded Downloader::downloadRobust(const std::string& fileUrl, const std::string& postUrl, size_t fIdx,
                                            const std::string& destDir, const std::string& filename) {
  auto attempt = [&](const std::string& u) { return http_.downloadFile(u, destDir, filename, 4, 500); };

  // Step 1: original URL
  try { return attempt(fileUrl); }
  catch (const Aborted&) { throw; }
  catch (const std::exception& e) { log("   -> [Retry] Initial URL failed: " + std::string(e.what())); }

  // Step 2: rotate img1..img10
  auto variants = subdomainVariants(fileUrl);
  if (!variants.empty()) {
    log("   -> [Retry] Trying alternative subdomains (img1..img10)...");
    for (auto& v : variants) {
      log("   -> Trying subdomain host " + hostOf(v) + " ...");
      try { return attempt(v); } catch (const Aborted&) { throw; } catch (...) {}
    }
  }

  // Step 3: refetch the post page for a fresh signed link
  log("   -> [Retry] Subdomains failed. Refetching post page (" + postUrl + ") for fresh link...");
  http_.sleepMs(500);
  try {
    auto doc = parseHtml(http_.fetchText(postUrl));
    auto media = extractPostMedia(doc.get(), postUrl);
    std::string fresh = fIdx < media.files.size() ? media.files[fIdx] : (media.files.empty() ? "" : media.files[0]);
    if (!fresh.empty()) {
      log("   -> Got fresh link from post page. Retrying download with 4 retries (500ms delay)...");
      try { return attempt(fresh); }
      catch (const Aborted&) { throw; }
      catch (const std::exception& e) { log("   -> [Retry] Fresh link initial attempt failed: " + std::string(e.what())); }
      for (auto& v : subdomainVariants(fresh)) {
        log("   -> Trying fresh link subdomain " + hostOf(v) + " ...");
        try { return attempt(v); } catch (const Aborted&) { throw; } catch (...) {}
      }
    }
  } catch (const Aborted&) { throw; }
  catch (const std::exception& e) { log("   -> [Retry] Error refetching post page: " + std::string(e.what())); }

  throw std::runtime_error("All download attempts, img1-10 subdomains, and post page refetch failed for " + filename);
}

void Downloader::downloadFileList(const std::string& startUrl) {
  CreatorInfo info = parseCreatorInfo(startUrl);
  Storage st(opts_.outputDir, info.username);
  st.init();
  creatorDir_ = st.creatorDir.string();

  log("==================================================");
  log("  MODE: Download File List");
  log("  Creator: " + info.username + " (" + info.service + ")");
  log("  Output Directory: " + creatorDir_);
  log("==================================================\n");

  std::vector<PageData> pages;
  std::set<std::string> visited;
  std::string url = startUrl;
  int pageNumber = 1;
  Stats stats;

  auto flush = [&] {
    std::vector<const PageData*> v;
    for (auto& p : pages) v.push_back(&p);
    st.writeList(info, v);
  };

  try {
    while (!url.empty()) {
      if (!visited.insert(url).second) { log("[Page " + std::to_string(pageNumber) + "] URL already visited, ending pagination."); break; }
      log("[Page " + std::to_string(pageNumber) + "] Fetching: " + url + " ...");
      if (cb_.current) cb_.current("Scanning page " + std::to_string(pageNumber), "", 0.f);

      std::unique_ptr<Node> doc;
      try { doc = parseHtml(http_.fetchText(url)); }
      catch (const Aborted&) { throw; }
      catch (const std::exception& e) { log("[Page " + std::to_string(pageNumber) + "] Error fetching page: " + e.what()); break; }

      PageData pd;
      pd.pageNumber = pageNumber;
      pd.pageUrl = url;
      pd.status = "completed";
      pd.posts = extractPostsFromPage(doc.get(), url);
      pd.postCount = (int)pd.posts.size();
      log("[Page " + std::to_string(pageNumber) + "] Found " + std::to_string(pd.posts.size()) + " posts.");
      stats.pages++;
      stats.posts += pd.postCount;
      pages.push_back(std::move(pd));
      if (cb_.stats) cb_.stats(stats);
      flush();

      std::string next = getNextPageUrl(doc.get(), url);
      if (!next.empty() && !visited.count(next)) {
        log("[Page " + std::to_string(pageNumber) + "] Next page found -> " + next);
        url = next;
        pageNumber++;
        if (opts_.delayMs > 0) http_.sleepMs(opts_.delayMs);
      } else {
        log("[Page " + std::to_string(pageNumber) + "] No further \"Next\" page link found. End of profile.");
        url.clear();
      }
    }
  } catch (const Aborted&) {
    flush();
    log("\n[Stopped] File list saved so far: " + st.listPath.string());
    return;
  }

  flush();
  log("\n==================================================");
  log("  File list generation completed!");
  log("  Total Pages Scanned: " + std::to_string(stats.pages));
  log("  Total Posts Found:   " + std::to_string(stats.posts));
  log("  File List (JSON):    " + st.listPath.string());
  log("==================================================\n");
  if (cb_.current) cb_.current("Completed", "", 1.f);
}

void Downloader::downloadContent(const std::string& startUrl) {
  CreatorInfo info = parseCreatorInfo(startUrl);
  Storage st(opts_.outputDir, info.username);
  st.init();
  creatorDir_ = st.creatorDir.string();

  ProgressState state;
  if (st.loadProgress(state) && !state.isFinished && !state.pages.empty()) {
    log("\n[Resume] Found existing progress for \"" + info.username + "\". Resuming download...");
    log("[Resume] Last updated: " + state.lastUpdated);
  } else {
    state = ProgressState();
    state.creator = info.username;
    state.service = info.service;
    state.sourceUrl = info.normalizedUrl;
    state.outputDir = creatorDir_;
    state.currentPageUrl = startUrl;
    std::vector<PageData> existing;
    if (st.loadExistingList(existing)) {
      int total = 0;
      for (auto& p : existing) {
        p.status = "pending";
        p.postCount = (int)p.posts.size();
        total += p.postCount;
        state.pages[std::to_string(p.pageNumber)] = p;
      }
      state.totalPagesScanned = (int)existing.size();
      state.totalPostsCount = total;
      log("\n[Init] Loaded existing file_list.json with " + std::to_string(existing.size()) + " pages and " + std::to_string(total) + " posts.");
    }
    st.saveProgress(state);
  }

  log("\n==================================================");
  log("  MODE: Download Content (Resumable)");
  log("  Creator: " + info.username + " (" + info.service + ")");
  log("  Files Destination: " + st.filesDir.string());
  log("==================================================\n");

  auto emitStats = [&] {
    if (!cb_.stats) return;
    Stats s;
    s.pages = (int)state.pages.size();
    for (auto& [k, p] : state.pages) s.posts += (int)p.posts.size();
    s.files = state.totalFilesDownloaded;
    s.bytes = state.totalBytesDownloaded;
    cb_.stats(s);
  };
  auto save = [&] { st.saveProgress(state); st.writeList(info, state.pages); };
  auto current = [&](const std::string& label, const std::string& count, float f) { if (cb_.current) cb_.current(label, count, f); };

  try {
    std::set<std::string> visited;
    std::string currentUrl = state.currentPageUrl.empty() ? startUrl : state.currentPageUrl;
    int pageNumber = state.currentPageNumber > 0 ? state.currentPageNumber : 1;

    while (!currentUrl.empty() && !stopped()) {
      if (!visited.insert(currentUrl).second) {
        log("[Page " + std::to_string(pageNumber) + "] Already visited (" + currentUrl + "), stopping pagination.");
        break;
      }
      state.currentPageNumber = pageNumber;
      state.currentPageUrl = currentUrl;
      const std::string pageKey = std::to_string(pageNumber);
      const std::string pageTag = "[Page " + pageKey + "]";

      std::unique_ptr<Node> doc;
      std::string nextUrl;
      auto pit = state.pages.find(pageKey);

      if (pit == state.pages.end() || pit->second.status != "completed") {
        log("\n--------------------------------------------------");
        log(pageTag + " Fetching listing: " + currentUrl);
        log("--------------------------------------------------");
        current("Page " + pageKey + ": loading listing...", "", 0.f);

        doc = parseHtml(http_.fetchText(currentUrl));
        auto scraped = extractPostsFromPage(doc.get(), currentUrl);
        nextUrl = getNextPageUrl(doc.get(), currentUrl);

        std::map<std::string, PostItem> existing;
        if (pit != state.pages.end()) for (auto& p : pit->second.posts) existing[p.id] = p;

        PageData pd;
        pd.pageNumber = pageNumber;
        pd.pageUrl = currentUrl;
        pd.status = "in_progress";
        for (auto& p : scraped) {
          auto e = existing.find(p.id);
          pd.posts.push_back(e != existing.end() ? e->second : p);
        }
        pd.postCount = (int)pd.posts.size();
        state.pages[pageKey] = std::move(pd);
        state.totalPagesScanned = (int)state.pages.size();
        save();
      } else {
        log(pageTag + " Cached in progress. Checking posts...");
      }

      PageData& page = state.pages[pageKey];
      const int total = (int)page.posts.size();
      emitStats();

      for (int i = 0; i < total && !stopped(); i++) {
        PostItem& post = page.posts[i];
        const int idx = i + 1;
        const std::string where = "[Page " + pageKey + " | Post " + std::to_string(idx) + "/" + std::to_string(total) + "]";
        const std::string count = "Page " + pageKey + " · post " + std::to_string(idx) + "/" + std::to_string(total);

        if (post.status == "completed" && !post.files.empty()) {
          bool allExist = std::all_of(post.files.begin(), post.files.end(), [](const PostFile& f) {
            return f.status == "completed" && !f.localPath.empty() && fs::exists(f.localPath);
          });
          if (allExist) {
            log(where + " [Skipped] Already downloaded: \"" + post.title + "\" (ID: " + post.id + ")");
            current("Skipped: " + post.title, count, (float)idx / total);
            continue;
          }
        }

        log("\n" + where + " Processing: \"" + post.title + "\" (ID: " + post.id + ")");
        log(" -> URL: " + post.url);
        current("Downloading: " + post.title, count, (float)(idx - 1) / total);
        post.status = "in_progress";
        st.saveProgress(state);

        try {
          auto postDoc = parseHtml(http_.fetchText(post.url));
          PostMedia media = extractPostMedia(postDoc.get(), post.url);
          if (!media.fullTitle.empty()) post.title = media.fullTitle;
          if (!media.description.empty()) post.description = media.description;
          log(" -> Found " + std::to_string(media.files.size()) + " file(s) in .post-body");

          std::vector<PostFile> postFiles;
          const std::string destDir = st.filesDir.string();

          for (size_t f = 0; f < media.files.size() && !stopped(); f++) {
            const std::string& fileUrl = media.files[f];
            std::string ext = fileExtension(fileUrl);
            std::string filename = media.files.size() == 1
                                       ? "[" + post.id + "] " + post.safeTitle + ext
                                       : "[" + post.id + "] " + post.safeTitle + "_" + std::to_string(f + 1) + ext;
            const std::string fileTag = "   [File " + std::to_string(f + 1) + "/" + std::to_string(media.files.size()) + "]";
            std::string targetPath = (st.filesDir / filename).string();

            auto prev = std::find_if(post.files.begin(), post.files.end(), [&](const PostFile& pf) { return pf.filename == filename; });
            if (prev != post.files.end() && prev->status == "completed" &&
                ((!prev->localPath.empty() && fs::exists(prev->localPath)) || fs::exists(targetPath))) {
              log(fileTag + " [Skipped] Already exists on disk: " + filename);
              if (prev->localPath.empty() || !fs::exists(prev->localPath)) prev->localPath = targetPath;
              postFiles.push_back(*prev);
              continue;
            }

            log(fileTag + " Downloading: " + filename + " ...");
            try {
              auto r = downloadRobust(fileUrl, post.url, f, destDir, filename);
              log("   -> Downloaded (" + formatBytes(r.size) + ")");
              PostFile pf;
              pf.url = fileUrl; pf.filename = filename; pf.size = r.size; pf.status = "completed"; pf.localPath = r.localPath;
              postFiles.push_back(pf);
              state.totalFilesDownloaded++;
              state.totalBytesDownloaded += r.size;
              emitStats();
            } catch (const Aborted&) { throw; }
            catch (const std::exception& e) {
              log("   -> [FAILED] " + std::string(e.what()));
              PostFile pf;
              pf.url = fileUrl; pf.filename = filename; pf.status = "failed"; pf.error = e.what();
              postFiles.push_back(pf);
            }
            post.files = postFiles;
            save();
          }

          bool failed = std::any_of(postFiles.begin(), postFiles.end(), [](const PostFile& f) { return f.status == "failed"; });
          if (stopped()) { post.files = postFiles; throw Aborted(); }
          post.status = failed ? "failed" : "completed";
        } catch (const Aborted&) { throw; }
        catch (const std::exception& e) {
          log(" -> [Error processing post " + post.id + "]: " + e.what());
          post.status = "failed";
          post.error = e.what();
        }

        save();
        current("Finished: " + post.title, count, (float)idx / total);
        if (opts_.delayMs > 0) http_.sleepMs(opts_.delayMs);
      }

      if (stopped()) break;

      page.status = std::all_of(page.posts.begin(), page.posts.end(), [](const PostItem& p) { return p.status == "completed"; })
                        ? "completed" : "in_progress";
      st.saveProgress(state);

      if (nextUrl.empty()) {
        if (!doc) {
          try { doc = parseHtml(http_.fetchText(currentUrl)); } catch (const Aborted&) { throw; } catch (...) {}
        }
        if (doc) nextUrl = getNextPageUrl(doc.get(), currentUrl);
      }
      if (!nextUrl.empty() && !visited.count(nextUrl)) {
        log("\n" + pageTag + " Finished page. Moving to Next -> " + nextUrl);
        currentUrl = nextUrl;
        pageNumber++;
        if (opts_.delayMs > 0) http_.sleepMs(opts_.delayMs);
      } else {
        log("\n" + pageTag + " No next page found. Profile crawl finished.");
        currentUrl.clear();
      }
    }
  } catch (const Aborted&) {
  }

  if (stopped()) {
    save();
    log("\n[Stopped] Progress saved. Start again to resume.");
    return;
  }

  state.isFinished = true;
  save();
  emitStats();
  log("\n==================================================");
  log("  Download Content Completed!");
  log("  Creator:           " + info.username);
  log("  Pages Scanned:     " + std::to_string(state.pages.size()));
  log("  Files Downloaded:  " + std::to_string(state.totalFilesDownloaded));
  log("  Total Downloaded:  " + formatBytes(state.totalBytesDownloaded));
  log("  Files Destination: " + st.filesDir.string());
  log("==================================================\n");
  current("Completed", "", 1.f);
}

}  // namespace cf
