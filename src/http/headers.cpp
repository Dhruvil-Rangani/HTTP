#include "http/headers.hpp"

#include <algorithm>

namespace httpd {
namespace {

constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

bool is_tchar(char c) noexcept {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

bool is_token(std::string_view s) noexcept {
  return !s.empty() && std::all_of(s.begin(), s.end(), is_tchar);
}

std::string_view trim_ows(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

Headers::Field* Headers::find(std::string_view name) {
  for (auto& f : fields_) {
    if (iequals(f.first, name)) return &f;
  }
  return nullptr;
}

const Headers::Field* Headers::find(std::string_view name) const {
  for (const auto& f : fields_) {
    if (iequals(f.first, name)) return &f;
  }
  return nullptr;
}

void Headers::set(std::string_view name, std::string_view value) {
  if (Field* f = find(name)) {
    f->second.assign(value);
  } else {
    fields_.emplace_back(std::string(name), std::string(value));
  }
}

void Headers::add(std::string_view name, std::string_view value) {
  if (Field* f = find(name)) {
    f->second.append(", ");
    f->second.append(value);
  } else {
    fields_.emplace_back(std::string(name), std::string(value));
  }
}

std::optional<std::string_view> Headers::get(std::string_view name) const {
  if (const Field* f = find(name)) return std::string_view(f->second);
  return std::nullopt;
}

bool Headers::remove(std::string_view name) {
  auto it = std::find_if(fields_.begin(), fields_.end(),
                         [&](const Field& f) { return iequals(f.first, name); });
  if (it == fields_.end()) return false;
  fields_.erase(it);
  return true;
}

bool Headers::has_token(std::string_view name, std::string_view token) const {
  auto value = get(name);
  if (!value) return false;
  std::string_view rest = *value;
  for (;;) {
    std::size_t comma = rest.find(',');
    if (iequals(trim_ows(rest.substr(0, comma)), token)) return true;
    if (comma == std::string_view::npos) return false;
    rest.remove_prefix(comma + 1);
  }
}

}  // namespace httpd
