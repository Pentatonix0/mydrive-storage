#include "common/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mydrive {
namespace {

class Writer {
 public:
  void u8(std::uint8_t value) { data_.push_back(value); }

  void u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    data_.push_back(static_cast<std::uint8_t>(value & 0xff));
  }

  void u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
  }

  void u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
  }

  void str(const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw ProtocolError("string is too large");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
  }

  std::vector<std::uint8_t> take() { return std::move(data_); }

 private:
  std::vector<std::uint8_t> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  std::uint8_t u8() {
    require(1);
    return data_[offset_++];
  }

  std::uint16_t u16() {
    require(2);
    std::uint16_t value = (static_cast<std::uint16_t>(data_[offset_]) << 8) |
                          static_cast<std::uint16_t>(data_[offset_ + 1]);
    offset_ += 2;
    return value;
  }

  std::uint32_t u32() {
    require(4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value = (value << 8) | data_[offset_ + i];
    }
    offset_ += 4;
    return value;
  }

  std::uint64_t u64() {
    require(8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | data_[offset_ + i];
    }
    offset_ += 8;
    return value;
  }

  std::string str() {
    const auto size = u32();
    require(size);
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
    offset_ += size;
    return value;
  }

  void done() const {
    if (offset_ != data_.size()) {
      throw ProtocolError("trailing bytes in message payload");
    }
  }

 private:
  void require(std::size_t size) const {
    if (offset_ + size > data_.size()) {
      throw ProtocolError("truncated message payload");
    }
  }

  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
};

}  // namespace

std::array<std::uint8_t, kFrameHeaderSize> encode_header(MessageType type, std::uint32_t payload_size) {
  Writer writer;
  writer.u32(kProtocolMagic);
  writer.u16(kProtocolVersion);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u32(payload_size);
  auto bytes = writer.take();
  std::array<std::uint8_t, kFrameHeaderSize> header{};
  std::copy(bytes.begin(), bytes.end(), header.begin());
  return header;
}

Frame decode_header(std::span<const std::uint8_t, kFrameHeaderSize> header) {
  Reader reader(header);
  const auto magic = reader.u32();
  const auto version = reader.u16();
  const auto type = reader.u16();
  const auto payload_size = reader.u32();
  reader.done();

  if (magic != kProtocolMagic) {
    throw ProtocolError("invalid protocol magic");
  }
  if (version != kProtocolVersion) {
    throw ProtocolError("unsupported protocol version");
  }
  if (payload_size > kMaxPayloadSize) {
    throw ProtocolError("payload is too large");
  }

  return Frame{static_cast<MessageType>(type), std::vector<std::uint8_t>(payload_size)};
}

std::vector<std::uint8_t> make_frame(MessageType type, const std::vector<std::uint8_t>& payload) {
  if (payload.size() > kMaxPayloadSize) {
    throw ProtocolError("payload is too large");
  }
  auto header = encode_header(type, static_cast<std::uint32_t>(payload.size()));
  std::vector<std::uint8_t> frame;
  frame.reserve(header.size() + payload.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::vector<std::uint8_t> serialize(const ManifestRequest& message) {
  Writer writer;
  writer.str(message.client_id);
  writer.u32(static_cast<std::uint32_t>(message.files.size()));
  for (const auto& file : message.files) {
    writer.str(file.name);
    writer.u64(file.size);
    writer.str(file.sha256);
  }
  return writer.take();
}

std::vector<std::uint8_t> serialize(const ManifestResponse& message) {
  Writer writer;
  writer.str(message.session_id);
  writer.u32(static_cast<std::uint32_t>(message.needed_files.size()));
  for (const auto& name : message.needed_files) {
    writer.str(name);
  }
  return writer.take();
}

std::vector<std::uint8_t> serialize(const UploadBegin& message) {
  Writer writer;
  writer.str(message.session_id);
  writer.str(message.client_id);
  writer.str(message.filename);
  writer.u64(message.size);
  writer.str(message.sha256);
  writer.u8(static_cast<std::uint8_t>(message.mode));
  return writer.take();
}

std::vector<std::uint8_t> serialize(const UploadFinished& message) {
  Writer writer;
  writer.str(message.filename);
  writer.u8(message.ok ? 1 : 0);
  writer.u64(message.bytes);
  writer.str(message.message);
  return writer.take();
}

std::vector<std::uint8_t> serialize_error(const std::string& message) {
  Writer writer;
  writer.str(message);
  return writer.take();
}

ManifestRequest parse_manifest_request(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  ManifestRequest message;
  message.client_id = reader.str();
  const auto count = reader.u32();
  message.files.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    FileInfo file;
    file.name = reader.str();
    file.size = reader.u64();
    file.sha256 = reader.str();
    message.files.push_back(std::move(file));
  }
  reader.done();
  return message;
}

ManifestResponse parse_manifest_response(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  ManifestResponse message;
  message.session_id = reader.str();
  const auto count = reader.u32();
  message.needed_files.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    message.needed_files.push_back(reader.str());
  }
  reader.done();
  return message;
}

UploadBegin parse_upload_begin(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  UploadBegin message;
  message.session_id = reader.str();
  message.client_id = reader.str();
  message.filename = reader.str();
  message.size = reader.u64();
  message.sha256 = reader.str();
  message.mode = static_cast<TransferMode>(reader.u8());
  reader.done();
  if (message.mode != TransferMode::Buffered && message.mode != TransferMode::Sendfile) {
    throw ProtocolError("invalid transfer mode");
  }
  return message;
}

UploadFinished parse_upload_finished(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  UploadFinished message;
  message.filename = reader.str();
  message.ok = reader.u8() != 0;
  message.bytes = reader.u64();
  message.message = reader.str();
  reader.done();
  return message;
}

std::string parse_error(std::span<const std::uint8_t> payload) {
  Reader reader(payload);
  auto message = reader.str();
  reader.done();
  return message;
}

std::string to_string(TransferMode mode) {
  switch (mode) {
    case TransferMode::Buffered:
      return "buffered";
    case TransferMode::Sendfile:
      return "sendfile";
  }
  return "unknown";
}

TransferMode parse_transfer_mode(const std::string& value) {
  if (value == "buffered") {
    return TransferMode::Buffered;
  }
  if (value == "sendfile" || value == "dma") {
    return TransferMode::Sendfile;
  }
  throw ProtocolError("unknown transfer mode: " + value);
}

}  // namespace mydrive
