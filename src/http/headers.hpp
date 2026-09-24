#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httpd {

// ASCII-only helpers: HTTP field names and tokens are ASCII by definition.
bool iequals(std::string_view a, std::string_view b) noexcept;
bool is_tchar(char c) noexcept;                // RFC 9110 §5.6.2
bool is_token(std::string_view s) noexcept;    // 1*tchar
std::string_view trim_ows(std::string_view s) noexcept;  // strip SP / HTAB

// Insertion-ordered, case-insensitive header list.
//
// A flat vector with linear search beats a hash map for the 5-20 fields of a
// typical message: no bucket allocations and the whole list stays within a few
// cache lines. Names keep the case they were given; lookups ignore case.
class Headers {
 public:
  using Field = std::pair<std::string, std::string>;

  // Replaces any existing value.
  void set(std::string_view name, std::string_view value);
  // Appends; a repeated name is folded into one comma-separated value
  // (RFC 9110 §5.3). Not suitable for Set-Cookie on the response side.
  void add(std::string_view name, std::string_view value);
  // The view is invalidated by any later mutation of this Headers object.
  std::optional<std::string_view> get(std::string_view name) const;
  bool has(std::string_view name) const { return find(name) != nullptr; }
  bool remove(std::string_view name);
  // True if the comma-separated list value of `name` contains `token`.
  bool has_token(std::string_view name, std::string_view token) const;

  std::size_t size() const noexcept { return fields_.size(); }
  bool empty() const noexcept { return fields_.empty(); }
  void clear() noexcept { fields_.clear(); }
  auto begin() const noexcept { return fields_.begin(); }
  auto end() const noexcept { return fields_.end(); }

 private:
  Field* find(std::string_view name);
  const Field* find(std::string_view name) const;

  std::vector<Field> fields_;
};

}  // namespace httpd
