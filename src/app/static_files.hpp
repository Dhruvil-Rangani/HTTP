#pragma once

#include <string>
#include <string_view>

#include "http/response.hpp"
#include "util/unique_fd.hpp"

namespace httpd {

// Serves regular files below a root directory, zero-copy via sendfile(2).
//
// Path traversal is blocked twice: the URL path is percent-decoded and
// normalized with ".." rejected, and the file is then opened with openat2(2)
// RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, so the kernel itself refuses to
// resolve anything outside the root, symlinks included.
class StaticFiles {
 public:
  explicit StaticFiles(const std::string& root);
  bool available() const noexcept { return root_.valid(); }
  // `rel_path` is the URL path below the mount point, e.g. "css/site.css".
  void serve(std::string_view rel_path, Response& res) const;

  // Exposed for unit tests: percent-decodes and normalizes a URL path into a
  // relative filesystem path. Returns false for anything that could escape.
  static bool sanitize(std::string_view url_path, std::string& out);

 private:
  UniqueFd root_;
};

}  // namespace httpd
