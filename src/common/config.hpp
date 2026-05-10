#pragma once

#include <filesystem>
#include <string>

namespace mydrive {

class JsonConfig {
 public:
  explicit JsonConfig(const std::filesystem::path& path);

  std::string string_value(const std::string& key, const std::string& fallback = "") const;
  unsigned int uint_value(const std::string& key, unsigned int fallback) const;

 private:
  std::string text_;
};

}  // namespace mydrive
