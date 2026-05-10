#pragma once

#include <filesystem>
#include <string>

namespace mydrive {

std::string sha256_file(const std::filesystem::path& path);
std::string sha256_hex(const unsigned char* data, unsigned int size);

}  // namespace mydrive
