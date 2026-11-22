#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cf {

struct Node {
  std::string tag;  // lowercase; "#document" for the root
  std::map<std::string, std::string> attrs;
  std::vector<std::unique_ptr<Node>> kids;
  Node* parent = nullptr;
  bool isText = false;
  std::string text;

  bool hasClass(const std::string& cls) const;
  bool hasAttr(const std::string& name) const { return attrs.count(name) > 0; }
  std::string attr(const std::string& name) const;
  std::string textContent() const;
  bool hasAncestorMatching(const std::function<bool(const Node&)>& pred) const;
};

using Pred = std::function<bool(const Node&)>;

std::unique_ptr<Node> parseHtml(const std::string& html);

/** All element descendants (document order) matching pred. */
std::vector<const Node*> findAll(const Node* root, const Pred& pred);
const Node* findFirst(const Node* root, const Pred& pred);

}  // namespace cf
