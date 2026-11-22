#pragma once
#include <string>

namespace cf {

std::string trim(const std::string& s);
std::string toLower(std::string s);
bool startsWith(const std::string& s, const std::string& prefix);
bool contains(const std::string& s, const std::string& needle);

/** Decode common HTML entities (named subset + numeric) to UTF-8. */
std::string decodeEntities(const std::string& s);

/** Make a post title safe for file names while keeping it readable. */
std::string sanitizeTitle(const std::string& raw, size_t maxLen = 100);

/** Resolve `ref` against `base` (like `new URL(ref, base)`); "" on failure. */
std::string resolveUrl(const std::string& base, const std::string& ref);

/** Split "scheme://host/path?q" into origin ("scheme://host") and path (no query). */
void splitUrl(const std::string& url, std::string& origin, std::string& path);

}  // namespace cf
