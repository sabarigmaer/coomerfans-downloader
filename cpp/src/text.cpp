#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <vector>

namespace cf {

static bool isWs(unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isWs(s[a])) a++;
  while (b > a && isWs(s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string toLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool contains(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

static void appendUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
  }
}

std::string decodeEntities(const std::string& s) {
  if (s.find('&') == std::string::npos) return s;
  static const std::vector<std::pair<std::string, std::string>> named = {
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
      {"hellip", "..."}, {"ndash", "-"}, {"mdash", "--"}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') { out += s[i]; continue; }
    size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
    std::string ent = s.substr(i + 1, semi - i - 1);
    bool done = false;
    if (!ent.empty() && ent[0] == '#') {
      try {
        unsigned cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? std::stoul(ent.substr(2), nullptr, 16)
                                                                           : std::stoul(ent.substr(1));
        appendUtf8(out, cp);
        done = true;
      } catch (...) {}
    } else {
      for (auto& [k, v] : named)
        if (ent == k) { out += v; done = true; break; }
    }
    if (done) i = semi; else out += s[i];
  }
  return out;
}

// Decode the UTF-8 sequence length from its lead byte.
static size_t u8len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

static std::string stripDotsAndSpace(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == '.' || isWs(s[a]))) a++;
  while (b > a && (s[b - 1] == '.' || isWs(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string sanitizeTitle(const std::string& raw, size_t maxLen) {
  if (trim(raw).empty()) return "untitled";
  std::string text = decodeEntities(raw);

  std::string out;
  bool lastSpace = false;
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    size_t n = std::min(u8len(c), text.size() - i);
    // Control chars: C0, DEL, and C1 (U+0080..U+009F = C2 80..C2 9F)
    if ((c < 0x20 && !isWs(c)) || c == 0x7F) { i++; continue; }
    if (c == 0xC2 && i + 1 < text.size() && (unsigned char)text[i + 1] >= 0x80 && (unsigned char)text[i + 1] <= 0x9F) { i += 2; continue; }
    const char* rep = nullptr;
    switch (c) {
      case '/': rep = "\xE2\x81\x84"; break;      // ⁄
      case '\\': rep = "\xEF\xBC\xBC"; break;     // ＼
      case ':': rep = "\xEF\xBC\x9A"; break;      // ：
      case '*': rep = "\xE2\x9C\xBE"; break;      // ✾
      case '?': rep = "\xEF\xBC\x9F"; break;      // ？
      case '"': rep = "\xE2\x80\x9D"; break;      // ”
      case '<': rep = "\xE3\x80\x8A"; break;      // 《
      case '>': rep = "\xE3\x80\x8B"; break;      // 》
      case '|': rep = "\xC2\xA6"; break;          // ¦
    }
    if (isWs(c)) {
      if (!lastSpace) out += ' ';
      lastSpace = true;
      i++;
      continue;
    }
    lastSpace = false;
    if (rep) out += rep; else out.append(text, i, n);
    i += n;
  }

  std::string res = stripDotsAndSpace(out);
  if (res.empty()) return "untitled";

  // Truncate by code points
  size_t cps = 0, i = 0;
  while (i < res.size() && cps < maxLen) { i += std::min(u8len(res[i]), res.size() - i); cps++; }
  if (i < res.size()) res = stripDotsAndSpace(res.substr(0, i));
  return res.empty() ? "untitled" : res;
}

void splitUrl(const std::string& url, std::string& origin, std::string& path) {
  origin.clear(); path.clear();
  size_t s = url.find("://");
  if (s == std::string::npos) return;
  size_t a = url.find_first_of("/?#", s + 3);
  if (a == std::string::npos) { origin = url; path = "/"; return; }
  origin = url.substr(0, a);
  size_t e = url.find_first_of("?#", a);
  path = url.substr(a, e == std::string::npos ? std::string::npos : e - a);
  if (path.empty() || path[0] != '/') path = "/" + path;
}

static std::string normalizePath(const std::string& p) {
  std::vector<std::string> segs;
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string seg = p.substr(i, j - i);
    bool last = j >= p.size();
    if (seg == "..") { if (!segs.empty()) segs.pop_back(); if (last) segs.push_back(""); }
    else if (seg == ".") { if (last) segs.push_back(""); }
    else segs.push_back(seg);
    i = j + 1;
  }
  std::string out;
  for (size_t k = 0; k < segs.size(); k++) { if (k) out += '/'; out += segs[k]; }
  if (out.empty() || out[0] != '/') out = "/" + out;
  return out;
}

std::string resolveUrl(const std::string& base, const std::string& refIn) {
  std::string ref = trim(refIn);
  static const std::regex schemeRe("^[A-Za-z][A-Za-z0-9+.-]*:");
  std::string result;
  std::string origin, bpath;
  splitUrl(base, origin, bpath);
  if (std::regex_search(ref, schemeRe)) {
    result = ref;
  } else if (origin.empty()) {
    return "";
  } else {
    size_t q = base.find_first_of("?#");
    std::string bquery;
    if (q != std::string::npos && base[q] == '?') { size_t h = base.find('#', q); bquery = base.substr(q, h == std::string::npos ? std::string::npos : h - q); }
    if (ref.empty()) result = origin + bpath + bquery;
    else if (startsWith(ref, "//")) result = origin.substr(0, origin.find(':') + 1) + ref;
    else if (ref[0] == '#') result = origin + bpath + bquery + ref;
    else if (ref[0] == '?') result = origin + bpath + ref;
    else {
      std::string tail;
      std::string rp = ref;
      size_t t = ref.find_first_of("?#");
      if (t != std::string::npos) { tail = ref.substr(t); rp = ref.substr(0, t); }
      std::string merged = rp.empty() || rp[0] == '/' ? rp : bpath.substr(0, bpath.rfind('/') + 1) + rp;
      result = origin + normalizePath(merged) + tail;
    }
  }
  std::string enc;
  for (char c : result) { if (c == ' ') enc += "%20"; else enc += c; }
  return enc;
}

}  // namespace cf
