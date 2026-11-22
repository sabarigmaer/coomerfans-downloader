#include "html.hpp"

#include <cctype>
#include <set>
#include <sstream>

#include "text.hpp"

namespace cf {

bool Node::hasClass(const std::string& cls) const {
  auto it = attrs.find("class");
  if (it == attrs.end()) return false;
  std::istringstream ss(it->second);
  std::string w;
  while (ss >> w) if (w == cls) return true;
  return false;
}

std::string Node::attr(const std::string& name) const {
  auto it = attrs.find(name);
  return it == attrs.end() ? "" : it->second;
}

std::string Node::textContent() const {
  if (isText) return text;
  std::string out;
  for (auto& k : kids) out += k->textContent();
  return out;
}

bool Node::hasAncestorMatching(const Pred& pred) const {
  for (const Node* p = parent; p; p = p->parent) if (pred(*p)) return true;
  return false;
}

static const std::set<std::string> kVoid = {"area", "base", "br", "col", "embed", "hr", "img",
                                            "input", "link", "meta", "source", "track", "wbr"};
static const std::set<std::string> kRaw = {"script", "style", "textarea", "title"};

std::unique_ptr<Node> parseHtml(const std::string& s) {
  auto root = std::make_unique<Node>();
  root->tag = "#document";
  Node* cur = root.get();
  const size_t n = s.size();
  const std::string ls = toLower(s);
  size_t i = 0;

  auto addText = [&](const std::string& t, bool raw) {
    if (t.empty()) return;
    auto x = std::make_unique<Node>();
    x->isText = true;
    x->text = raw ? t : decodeEntities(t);
    x->parent = cur;
    cur->kids.push_back(std::move(x));
  };

  while (i < n) {
    if (s[i] != '<') {
      size_t j = s.find('<', i);
      if (j == std::string::npos) j = n;
      addText(s.substr(i, j - i), false);
      i = j;
      continue;
    }
    if (s.compare(i, 4, "<!--") == 0) {
      size_t j = s.find("-->", i + 4);
      i = j == std::string::npos ? n : j + 3;
      continue;
    }
    if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
      size_t j = s.find('>', i);
      i = j == std::string::npos ? n : j + 1;
      continue;
    }
    if (i + 1 < n && s[i + 1] == '/') {
      size_t j = s.find('>', i);
      if (j == std::string::npos) break;
      std::string name = toLower(trim(s.substr(i + 2, j - i - 2)));
      for (Node* p = cur; p && p->parent; p = p->parent)
        if (p->tag == name) { cur = p->parent; break; }
      i = j + 1;
      continue;
    }
    if (i + 1 < n && std::isalpha((unsigned char)s[i + 1])) {
      size_t j = i + 1;
      while (j < n && !std::isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') j++;
      auto node = std::make_unique<Node>();
      node->tag = ls.substr(i + 1, j - i - 1);
      bool selfClose = false;
      while (j < n && s[j] != '>') {
        if (std::isspace((unsigned char)s[j])) { j++; continue; }
        if (s[j] == '/') { selfClose = true; j++; continue; }
        selfClose = false;
        size_t a = j;
        while (j < n && !std::isspace((unsigned char)s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/') j++;
        std::string name = ls.substr(a, j - a);
        while (j < n && std::isspace((unsigned char)s[j])) j++;
        std::string val;
        if (j < n && s[j] == '=') {
          j++;
          while (j < n && std::isspace((unsigned char)s[j])) j++;
          if (j < n && (s[j] == '"' || s[j] == '\'')) {
            char q = s[j++];
            size_t e = s.find(q, j);
            if (e == std::string::npos) e = n;
            val = s.substr(j, e - j);
            j = e + 1;
          } else {
            size_t e = j;
            while (e < n && !std::isspace((unsigned char)s[e]) && s[e] != '>') e++;
            val = s.substr(j, e - j);
            j = e;
          }
        }
        if (!name.empty()) node->attrs[name] = decodeEntities(val);
      }
      i = j < n ? j + 1 : n;

      std::string tag = node->tag;
      if ((tag == "p" && cur->tag == "p") || (tag == "li" && cur->tag == "li")) cur = cur->parent;
      node->parent = cur;
      Node* raw = node.get();
      cur->kids.push_back(std::move(node));

      if (kRaw.count(tag) && !selfClose) {
        size_t e = ls.find("</" + tag, i);
        if (e == std::string::npos) e = n;
        Node* save = cur;
        cur = raw;
        addText(s.substr(i, e - i), true);
        cur = save;
        size_t gt = s.find('>', e);
        i = gt == std::string::npos ? n : gt + 1;
      } else if (!kVoid.count(tag) && !selfClose) {
        cur = raw;
      }
      continue;
    }
    addText("<", true);
    i++;
  }
  return root;
}

static void collect(const Node* node, const Pred& pred, std::vector<const Node*>& out, bool firstOnly) {
  for (auto& k : node->kids) {
    if (k->isText) continue;
    if (pred(*k)) {
      out.push_back(k.get());
      if (firstOnly) return;
    }
    collect(k.get(), pred, out, firstOnly);
    if (firstOnly && !out.empty()) return;
  }
}

std::vector<const Node*> findAll(const Node* root, const Pred& pred) {
  std::vector<const Node*> out;
  if (root) collect(root, pred, out, false);
  return out;
}

const Node* findFirst(const Node* root, const Pred& pred) {
  std::vector<const Node*> out;
  if (root) collect(root, pred, out, true);
  return out.empty() ? nullptr : out[0];
}

}  // namespace cf
