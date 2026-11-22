#pragma once
#include <string>
#include <vector>

#include "html.hpp"
#include "types.hpp"

namespace cf {

CreatorInfo parseCreatorInfo(const std::string& rawUrl);
std::vector<PostItem> extractPostsFromPage(const Node* doc, const std::string& pageUrl);
std::string getNextPageUrl(const Node* doc, const std::string& currentUrl);  // "" if none

struct PostMedia {
  std::vector<std::string> files;
  std::string fullTitle, description;
};
PostMedia extractPostMedia(const Node* doc, const std::string& postUrl);

}  // namespace cf
