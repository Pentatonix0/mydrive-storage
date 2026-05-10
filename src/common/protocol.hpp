#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mydrive {

constexpr std::uint32_t kProtocolMagic = 0x4d594452; // MYDR
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kFrameHeaderSize = 12;
constexpr std::uint32_t kMaxPayloadSize = 16 * 1024 * 1024;

enum class MessageType : std::uint16_t {
  ManifestRequest = 1,
  ManifestResponse = 2,
  UploadBegin = 3,
  UploadReady = 4,
  UploadFinished = 5,
  Error = 6,
};

enum class TransferMode : std::uint8_t {
  Buffered = 1,
  Sendfile = 2,
};

struct FileInfo {
  std::string name;
  std::uint64_t size = 0;
  std::string sha256;
};

struct ManifestRequest {
  std::string client_id;
  std::vector<FileInfo> files;
};

struct ManifestResponse {
  std::string session_id;
  std::vector<std::string> needed_files;
};

struct UploadBegin {
  std::string session_id;
  std::string client_id;
  std::string filename;
  std::uint64_t size = 0;
  std::string sha256;
  TransferMode mode = TransferMode::Buffered;
};

struct UploadFinished {
  std::string filename;
  bool ok = false;
  std::uint64_t bytes = 0;
  std::string message;
};

struct Frame {
  MessageType type;
  std::vector<std::uint8_t> payload;
};

class ProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::array<std::uint8_t, kFrameHeaderSize> encode_header(MessageType type, std::uint32_t payload_size);
Frame decode_header(std::span<const std::uint8_t, kFrameHeaderSize> header);
std::vector<std::uint8_t> make_frame(MessageType type, const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> serialize(const ManifestRequest& message);
std::vector<std::uint8_t> serialize(const ManifestResponse& message);
std::vector<std::uint8_t> serialize(const UploadBegin& message);
std::vector<std::uint8_t> serialize(const UploadFinished& message);
std::vector<std::uint8_t> serialize_error(const std::string& message);

ManifestRequest parse_manifest_request(std::span<const std::uint8_t> payload);
ManifestResponse parse_manifest_response(std::span<const std::uint8_t> payload);
UploadBegin parse_upload_begin(std::span<const std::uint8_t> payload);
UploadFinished parse_upload_finished(std::span<const std::uint8_t> payload);
std::string parse_error(std::span<const std::uint8_t> payload);

std::string to_string(TransferMode mode);
TransferMode parse_transfer_mode(const std::string& value);

}  // namespace mydrive
