#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cf {

// Field names and JSON shape match the TypeScript CLI so progress.json /
// file_list.json can be resumed by either program.

struct PostFile {
  std::string url, filename;
  long long size = 0;
  std::string status = "pending";  // pending | downloading | completed | failed
  std::string localPath, error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PostFile, url, filename, size, status, localPath, error)

struct PostItem {
  std::string id, title, safeTitle, url, date, description;
  std::vector<PostFile> files;
  std::string status = "pending";  // pending | in_progress | completed | failed
  std::string error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PostItem, id, title, safeTitle, url, date, description, files, status, error)

struct PageData {
  int pageNumber = 0;
  std::string pageUrl;
  int postCount = 0;
  std::string status = "pending";  // pending | in_progress | completed
  std::vector<PostItem> posts;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PageData, pageNumber, pageUrl, postCount, status, posts)

struct CreatorInfo {
  std::string service, userId, username, normalizedUrl;
};

struct ProgressState {
  std::string creator, service, sourceUrl, outputDir;
  int currentPageNumber = 1;
  std::string currentPageUrl;
  int totalPagesScanned = 0, totalPostsCount = 0, totalFilesDownloaded = 0;
  long long totalBytesDownloaded = 0;
  bool isFinished = false;
  std::string lastUpdated;
  std::map<std::string, PageData> pages;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProgressState, creator, service, sourceUrl, outputDir, currentPageNumber,
                                                currentPageUrl, totalPagesScanned, totalPostsCount, totalFilesDownloaded,
                                                totalBytesDownloaded, isFinished, lastUpdated, pages)

}  // namespace cf
