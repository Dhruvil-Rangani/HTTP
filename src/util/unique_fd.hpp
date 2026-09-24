#pragma once

#include <unistd.h>

namespace httpd {

// RAII owner of a POSIX file descriptor. Move-only, so ownership of sockets,
// epoll/eventfd handles and open files is always explicit and a descriptor is
// closed exactly once, even on early returns and exceptions.
class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  int release() noexcept {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace httpd
