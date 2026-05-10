#include "common/config.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace mydrive {

JsonConfig::JsonConfig(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open config: " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  text_ = buffer.str();
}

std::string JsonConfig::string_value(const std::string& key, const std::string& fallback) const {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (std::regex_search(text_, match, pattern)) {
    return match[1].str();
  }
  return fallback;
}

unsigned int JsonConfig::uint_value(const std::string& key, unsigned int fallback) const {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (std::regex_search(text_, match, pattern)) {
    return static_cast<unsigned int>(std::stoul(match[1].str()));
  }
  return fallback;
}

}  // namespace mydrive
