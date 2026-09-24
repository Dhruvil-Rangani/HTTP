#include "app/static_files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define HTTPD_HAVE_OPENAT2 1
#endif

namespace httpd {
namespace {

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int open_beneath(int dirfd, const std::string& path) {
#if defined(HTTPD_HAVE_OPENAT2) && defined(SYS_openat2)
  open_how how{};
  how.flags = O_RDONLY | O_CLOEXEC;
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
  int fd = static_cast<int>(::syscall(SYS_openat2, dirfd, path.c_str(), &how, sizeof how));
  if (fd >= 0 || errno != ENOSYS) return fd;
#endif
  // Pre-5.6 kernels: sanitize() already rejected "..", and O_NOFOLLOW at
  // least refuses a symlink as the final component.
  return ::openat(dirfd, path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

std::string_view mime_type(std::string_view path) {
  std::size_t dot = path.rfind('.');
  std::string_view ext = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
  if (ext == "json") return "application/json";
  if (ext == "txt" || ext == "log") return "text/plain; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "ico") return "image/x-icon";
  if (ext == "wasm") return "application/wasm";
  if (ext == "pdf") return "application/pdf";
  return "application/octet-stream";
}

}  // namespace

StaticFiles::StaticFiles(const std::string& root)
    : root_(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}

bool StaticFiles::sanitize(std::string_view url_path, std::string& out) {
  std::string decoded;
  decoded.reserve(url_path.size());
  for (std::size_t i = 0; i < url_path.size(); ++i) {
    char c = url_path[i];
    if (c == '%') {
      if (i + 2 >= url_path.size()) return false;
      int hi = hex_digit(url_path[i + 1]);
      int lo = hex_digit(url_path[i + 2]);
      if (hi < 0 || lo < 0) return false;
      c = static_cast<char>(hi * 16 + lo);
      i += 2;
    }
    if (c == '\0' || c == '\\') return false;
    decoded.push_back(c);
  }

  out.clear();
  std::size_t pos = 0;
  while (pos <= decoded.size()) {
    std::size_t slash = decoded.find('/', pos);
    if (slash == std::string::npos) slash = decoded.size();
    std::string_view segment(decoded.data() + pos, slash - pos);
    if (segment == "..") return false;
    if (!segment.empty() && segment != ".") {
      if (!out.empty()) out.push_back('/');
      out.append(segment);
    }
    pos = slash + 1;
  }
  return true;
}

void StaticFiles::serve(std::string_view rel_path, Response& res) const {
  if (!root_.valid()) {
    res = Response::text(404, "static root not available\n");
    return;
  }
  std::string path;
  if (!sanitize(rel_path, path)) {
    res = Response::text(400, "invalid path\n");
    return;
  }
  if (path.empty()) path = "index.html";

  UniqueFd file(open_beneath(root_.get(), path));
  struct stat st {};
  if (file.valid() && ::fstat(file.get(), &st) == 0 && S_ISDIR(st.st_mode)) {
    path += "/index.html";
    file.reset(open_beneath(root_.get(), path));
  }
  if (!file.valid() || ::fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    // ENOENT, ELOOP (symlink), EXDEV (escape attempt), EACCES... all look the
    // same from outside so the response does not leak the filesystem layout.
    res = Response::text(404, "not found\n");
    return;
  }

  res.status = 200;
  res.headers.set("Content-Type", mime_type(path));
  res.file = std::move(file);
  res.file_offset = 0;
  res.file_length = static_cast<std::uint64_t>(st.st_size);
}

}  // namespace httpd
