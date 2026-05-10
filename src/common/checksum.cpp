#include "common/checksum.hpp"

#include <openssl/evp.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mydrive {

std::string sha256_hex(const unsigned char* data, unsigned int size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<int>(data[i]);
  }
  return out.str();
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open file for checksum: " + path.string());
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    throw std::runtime_error("EVP_MD_CTX_new failed");
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = input.gcount();
    if (read > 0 && EVP_DigestUpdate(ctx, buffer.data(), static_cast<std::size_t>(read)) != 1) {
      EVP_MD_CTX_free(ctx);
      throw std::runtime_error("EVP_DigestUpdate failed");
    }
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(ctx, digest.data(), &digest_size) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }

  EVP_MD_CTX_free(ctx);
  return sha256_hex(digest.data(), digest_size);
}

}  // namespace mydrive
