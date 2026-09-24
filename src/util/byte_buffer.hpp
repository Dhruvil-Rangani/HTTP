#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>

namespace httpd {

// Growable input buffer for socket reads.
//
//   [ consumed | readable (rpos..wpos) | writable (wpos..cap) ]
//
// Unlike std::string::resize() it never zero-fills the region handed to
// recv(2), and consumed bytes are reclaimed lazily by sliding the readable
// region to the front only when the tail runs out of room.
class ByteBuffer {
 public:
  std::string_view readable() const noexcept { return {data_.get() + rpos_, wpos_ - rpos_}; }
  std::size_t size() const noexcept { return wpos_ - rpos_; }
  bool empty() const noexcept { return rpos_ == wpos_; }

  void consume(std::size_t n) noexcept {
    rpos_ += n;
    if (rpos_ == wpos_) rpos_ = wpos_ = 0;  // cheap reset when drained
  }

  // Returns a pointer to at least `n` writable bytes; call commit() after
  // writing into it.
  char* prepare(std::size_t n) {
    if (cap_ - wpos_ >= n) return data_.get() + wpos_;
    std::size_t live = size();
    if (rpos_ > 0 && cap_ - live >= n) {
      std::memmove(data_.get(), data_.get() + rpos_, live);  // compact in place
    } else {
      std::size_t new_cap = cap_ ? cap_ : 4096;
      while (new_cap - live < n) new_cap *= 2;
      std::unique_ptr<char[]> grown(new char[new_cap]);
      if (live) std::memcpy(grown.get(), data_.get() + rpos_, live);
      data_ = std::move(grown);
      cap_ = new_cap;
    }
    rpos_ = 0;
    wpos_ = live;
    return data_.get() + wpos_;
  }

  std::size_t writable() const noexcept { return cap_ - wpos_; }
  void commit(std::size_t n) noexcept { wpos_ += n; }

 private:
  std::unique_ptr<char[]> data_;
  std::size_t cap_ = 0;
  std::size_t rpos_ = 0;
  std::size_t wpos_ = 0;
};

}  // namespace httpd
