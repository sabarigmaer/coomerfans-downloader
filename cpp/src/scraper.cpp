#include "scraper.hpp"

#include <regex>
#include <set>
#include <stdexcept>

#include "text.hpp"

namespace cf {

CreatorInfo parseCreatorInfo(const std::string& rawUrl) {
  std::string origin, path;
  splitUrl(rawUrl, origin, path);
  if (origin.empty()) throw std::runtime_error("Invalid URL: " + rawUrl);

  std::vector<std::string> seg;
  size_t i = 0;
  while (i < path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) seg.push_back(path.substr(i, j - i));
    i = j + 1;
  }

  CreatorInfo info;
  info.service = "unknown";
  if (!seg.empty() && seg[0] == "u") {
    if (seg.size() >= 4) { info.service = seg[1]; info.userId = seg[2]; info.username = seg[3]; }
    else if (seg.size() == 3) { info.service = seg[1]; info.username = seg[2]; info.userId = seg[2]; }
    else if (seg.size() == 2) { info.username = seg[1]; info.userId = seg[1]; }
  }
  if (info.username.empty()) info.username = seg.empty() ? "creator" : seg.back();
  if (info.username == "." || info.username == "..") info.username = "creator";  // never escape the output dir
  info.normalizedUrl = origin + path;
  return info;
}

static bool isTag(const Node& n, const char* t) { return n.tag == t; }

std::vector<PostItem> extractPostsFromPage(const Node* doc, const std::string& pageUrl) {
  std::vector<PostItem> posts;
  auto cards = findAll(doc, [](const Node& n) { return n.hasClass("post") || n.hasClass("post-card"); });

  for (const Node* card : cards) {
    const Node* h3 = findFirst(card, [](const Node& n) { return isTag(n, "h3"); });
    const Node* link = h3 ? findFirst(h3, [](const Node& n) { return isTag(n, "a"); }) : nullptr;
    if (!link) link = findFirst(card, [](const Node& n) { return isTag(n, "a") && n.hasClass("view-post"); });
    if (!link) link = findFirst(card, [](const Node& n) { return isTag(n, "a") && contains(n.attr("href"), "/p/"); });
    if (!link) continue;

    std::string href = link->attr("href");
    if (href.empty()) continue;
    std::string fullUrl = resolveUrl(pageUrl, href);
    if (fullUrl.empty()) continue;

    std::string id;
    size_t p = href.find("/p/");
    if (p != std::string::npos) {
      size_t e = href.find('/', p + 3);
      id = href.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3);
    } else {
      for (char c : href) id += (std::isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }

    std::string rawTitle = trim(h3 ? h3->textContent() : link->textContent());
    std::string title = decodeEntities(rawTitle);
    if (title.empty()) title = "Post " + id;

    const Node* dateEl = findFirst(card, [](const Node& n) { return n.hasClass("post-date"); });
    const Node* pEl = findFirst(card, [](const Node& n) { return isTag(n, "p"); });

    PostItem item;
    item.id = id;
    item.title = title;
    item.safeTitle = sanitizeTitle(title);
    item.url = fullUrl;
    item.date = dateEl ? trim(dateEl->textContent()) : "";
    item.description = pEl ? trim(pEl->textContent()) : "";
    posts.push_back(std::move(item));
  }
  return posts;
}

std::string getNextPageUrl(const Node* doc, const std::string& currentUrl) {
  const Node* next = findFirst(doc, [](const Node& n) {
    return isTag(n, "a") && (n.hasClass("next") || n.attr("rel") == "next");
  });
  if (!next) {
    next = findFirst(doc, [](const Node& n) {
      if (!isTag(n, "a")) return false;
      bool inNav = n.hasAncestorMatching([](const Node& a) { return a.hasClass("pagination") || isTag(a, "nav"); });
      if (!inNav) return false;
      std::string t = toLower(trim(n.textContent()));
      return t == "next" || startsWith(t, "next ");
    });
  }
  if (!next) return "";
  std::string href = next->attr("href");
  if (href.empty()) return "";
  std::string url = resolveUrl(currentUrl, href);
  return url == currentUrl ? "" : url;
}

PostMedia extractPostMedia(const Node* doc, const std::string& postUrl) {
  PostMedia out;
  const Node* body = findFirst(doc, [](const Node& n) { return n.hasClass("post-body"); });
  if (!body) body = findFirst(doc, [](const Node& n) { return n.hasClass("post-wrap"); });
  if (!body) body = findFirst(doc, [](const Node& n) { return isTag(n, "body"); });
  if (!body) return out;

  if (const Node* h1 = findFirst(doc, [](const Node& n) { return isTag(n, "h1"); })) {
    std::string t = trim(h1->textContent());
    if (!t.empty()) out.fullTitle = decodeEntities(t);
  }
  auto inWrap = [](const Node& a) { return a.hasClass("post-wrap") || a.hasClass("post-body"); };
  if (const Node* p = findFirst(doc, [&](const Node& n) { return isTag(n, "p") && n.hasAncestorMatching(inWrap); }))
    out.description = trim(p->textContent());

  std::set<std::string> seen;
  auto add = [&](const std::string& src) {
    if (src.empty() || startsWith(src, "data:")) return;
    std::string u = resolveUrl(postUrl, src);
    if (!u.empty() && seen.insert(u).second) out.files.push_back(u);
  };
  auto firstAttr = [](const Node& n, std::initializer_list<const char*> names) {
    for (auto nm : names) { std::string v = n.attr(nm); if (!v.empty()) return v; }
    return std::string();
  };

  for (const Node* img : findAll(body, [](const Node& n) { return isTag(n, "img"); })) {
    std::string src = firstAttr(*img, {"src", "data-src", "data-url"});
    if (!contains(src, "creative.tscprts.com")) add(src);
  }
  for (const char* media : {"video", "audio"}) {
    for (const Node* m : findAll(body, [&](const Node& n) { return n.tag == media; })) {
      add(firstAttr(*m, {"src", "data-src"}));
      for (const Node* s : findAll(m, [](const Node& n) { return isTag(n, "source"); }))
        add(firstAttr(*s, {"src", "data-src"}));
    }
  }
  static const std::regex mediaExt(
      R"(\.(jpe?g|png|gif|webp|avif|mp4|webm|m4v|mov|mkv|zip|rar|7z|tar|gz|pdf|mp3|flac|wav)(\?.*)?$)",
      std::regex::icase);
  for (const Node* a : findAll(body, [](const Node& n) { return isTag(n, "a"); })) {
    std::string href = a->attr("href");
    if (href.empty()) continue;
    if (contains(href, "/storage/") || std::regex_search(href, mediaExt) || a->hasAttr("download")) add(href);
  }
  return out;
}

}  // namespace cf
