#include "common/checksum.hpp"
#include "common/config.hpp"
#include "common/protocol.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(MYDRIVE_PLATFORM_LINUX)
#include <sys/sendfile.h>
#elif defined(MYDRIVE_PLATFORM_MACOS)
#include <sys/socket.h>
#endif

namespace fs = std::filesystem;
using boost::asio::ip::tcp;

namespace mydrive {
namespace {

struct ClientConfig {
  std::string client_id;
  std::string server_host;
  unsigned int server_port;
  fs::path directory;
  unsigned int max_connections;
  TransferMode transfer_mode;
};

struct SyncStats {
  std::size_t files_uploaded = 0;
  std::uint64_t bytes_uploaded = 0;
  double elapsed_seconds = 0.0;
};

class FileDescriptor {
 public:
  explicit FileDescriptor(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY)) {
    if (fd_ < 0) {
      throw std::runtime_error("cannot open file: " + path.string());
    }
  }

  ~FileDescriptor() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return fd_; }

 private:
  int fd_;
};

void write_frame_sync(tcp::socket& socket, MessageType type, const std::vector<std::uint8_t>& payload) {
  const auto frame = make_frame(type, payload);
  boost::asio::write(socket, boost::asio::buffer(frame));
}

Frame read_frame_sync(tcp::socket& socket) {
  std::array<std::uint8_t, kFrameHeaderSize> header{};
  boost::asio::read(socket, boost::asio::buffer(header));
  auto frame = decode_header(std::span<const std::uint8_t, kFrameHeaderSize>(header));
  if (!frame.payload.empty()) {
    boost::asio::read(socket, boost::asio::buffer(frame.payload));
  }
  return frame;
}

tcp::socket connect_to_server(boost::asio::io_context& io, const ClientConfig& config) {
  tcp::resolver resolver(io);
  auto endpoints = resolver.resolve(config.server_host, std::to_string(config.server_port));
  tcp::socket socket(io);
  boost::asio::connect(socket, endpoints);
  return socket;
}

std::string transfer_mode_arg(int argc, char** argv, const std::string& fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--mode") {
      return argv[i + 1];
    }
  }
  return fallback;
}

fs::path config_path_from_args(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--config") {
      return argv[i + 1];
    }
  }
  return "config/client.json";
}

bool has_arg(int argc, char** argv, const std::string& name) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == name) {
      return true;
    }
  }
  return false;
}

ClientConfig read_config(int argc, char** argv) {
  JsonConfig config(config_path_from_args(argc, argv));
  ClientConfig result;
  result.client_id = config.string_value("client_id", "default-client");
  result.server_host = config.string_value("server_host", "127.0.0.1");
  result.server_port = config.uint_value("server_port", 9000);
  result.directory = config.string_value("directory", "./client_files");
  result.max_connections = std::clamp(config.uint_value("max_connections", 1), 1u, 32u);
  result.transfer_mode = parse_transfer_mode(
      transfer_mode_arg(argc, argv, config.string_value("transfer_mode", "buffered")));
  return result;
}

std::vector<FileInfo> scan_directory(const fs::path& directory) {
  if (!fs::exists(directory)) {
    throw std::runtime_error("directory does not exist: " + directory.string());
  }
  if (!fs::is_directory(directory)) {
    throw std::runtime_error("not a directory: " + directory.string());
  }

  std::vector<FileInfo> files;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    FileInfo file;
    file.name = entry.path().filename().string();
    file.size = fs::file_size(entry.path());
    file.sha256 = sha256_file(entry.path());
    files.push_back(std::move(file));
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return files;
}

std::string human_bytes(std::uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(unit == 0 ? 0 : 2);
  out << value << ' ' << units[unit];
  return out.str();
}

void send_buffered(tcp::socket& socket, const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open file: " + path.string());
  }

  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto n = input.gcount();
    if (n > 0) {
      boost::asio::write(socket, boost::asio::buffer(buffer.data(), static_cast<std::size_t>(n)));
    }
  }
}

void send_sendfile(tcp::socket& socket, const fs::path& path, std::uint64_t size) {
  FileDescriptor file(path);
  std::uint64_t remaining = size;

#if defined(MYDRIVE_PLATFORM_LINUX)
  off_t offset = 0;
  while (remaining > 0) {
    const auto chunk = static_cast<size_t>(std::min<std::uint64_t>(remaining, 64ull * 1024ull * 1024ull));
    const auto sent = ::sendfile(socket.native_handle(), file.get(), &offset, chunk);
    if (sent < 0) {
      throw std::runtime_error("sendfile failed for " + path.string());
    }
    if (sent == 0) {
      throw std::runtime_error("sendfile sent zero bytes for " + path.string());
    }
    remaining -= static_cast<std::uint64_t>(sent);
  }
#elif defined(MYDRIVE_PLATFORM_MACOS)
  off_t offset = 0;
  while (remaining > 0) {
    off_t len = static_cast<off_t>(std::min<std::uint64_t>(remaining, 64ull * 1024ull * 1024ull));
    const int rc = ::sendfile(file.get(), socket.native_handle(), offset, &len, nullptr, 0);
    if (len > 0) {
      offset += len;
      remaining -= static_cast<std::uint64_t>(len);
    }
    if (rc != 0 && len == 0) {
      throw std::runtime_error("sendfile failed for " + path.string());
    }
  }
#else
  (void)size;
  send_buffered(socket, path);
#endif
}

void upload_one_file(const ClientConfig& config,
                     const std::string& session_id,
                     const FileInfo& file,
                     std::atomic<std::uint64_t>& uploaded_bytes,
                     std::atomic<std::size_t>& uploaded_files,
                     std::mutex& cout_mutex) {
  boost::asio::io_context io;
  auto socket = connect_to_server(io, config);

  UploadBegin begin;
  begin.session_id = session_id;
  begin.client_id = config.client_id;
  begin.filename = file.name;
  begin.size = file.size;
  begin.sha256 = file.sha256;
  begin.mode = config.transfer_mode;

  const auto started = std::chrono::steady_clock::now();
  write_frame_sync(socket, MessageType::UploadBegin, serialize(begin));

  auto frame = read_frame_sync(socket);
  if (frame.type == MessageType::Error) {
    throw std::runtime_error(parse_error(frame.payload));
  }
  if (frame.type != MessageType::UploadReady) {
    throw std::runtime_error("server did not accept upload: " + file.name);
  }

  const auto path = config.directory / file.name;
  if (config.transfer_mode == TransferMode::Sendfile) {
    send_sendfile(socket, path, file.size);
  } else {
    send_buffered(socket, path);
  }

  frame = read_frame_sync(socket);
  if (frame.type == MessageType::Error) {
    throw std::runtime_error(parse_error(frame.payload));
  }
  if (frame.type != MessageType::UploadFinished) {
    throw std::runtime_error("unexpected server response after upload");
  }

  const auto result = parse_upload_finished(frame.payload);
  if (!result.ok) {
    throw std::runtime_error("upload failed for " + file.name + ": " + result.message);
  }

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  uploaded_bytes.fetch_add(file.size);
  uploaded_files.fetch_add(1);

  std::lock_guard lock(cout_mutex);
  const auto mbps = elapsed > 0.0 ? (static_cast<double>(file.size) * 8.0 / 1000.0 / 1000.0 / elapsed) : 0.0;
  std::cout << "[upload] " << file.name << "  " << human_bytes(file.size) << "  done  ";
  std::cout.setf(std::ios::fixed);
  std::cout.precision(2);
  std::cout << mbps << " Mbps\n";
}

ManifestResponse send_manifest(const ClientConfig& config, const std::vector<FileInfo>& files) {
  boost::asio::io_context io;
  auto socket = connect_to_server(io, config);

  ManifestRequest request;
  request.client_id = config.client_id;
  request.files = files;

  write_frame_sync(socket, MessageType::ManifestRequest, serialize(request));
  const auto frame = read_frame_sync(socket);
  if (frame.type == MessageType::Error) {
    throw std::runtime_error(parse_error(frame.payload));
  }
  if (frame.type != MessageType::ManifestResponse) {
    throw std::runtime_error("unexpected server response to manifest");
  }
  return parse_manifest_response(frame.payload);
}

SyncStats synchronize(const ClientConfig& config) {
  const auto started = std::chrono::steady_clock::now();
  std::cout << "Scanning directory...\n";
  auto files = scan_directory(config.directory);

  std::uint64_t total_bytes = 0;
  for (const auto& file : files) {
    total_bytes += file.size;
  }
  std::cout << "Found " << files.size() << " files, total " << human_bytes(total_bytes) << '\n';

  auto response = send_manifest(config, files);
  std::unordered_map<std::string, FileInfo> by_name;
  for (const auto& file : files) {
    by_name.emplace(file.name, file);
  }

  std::vector<FileInfo> needed;
  std::uint64_t needed_bytes = 0;
  for (const auto& name : response.needed_files) {
    auto it = by_name.find(name);
    if (it != by_name.end()) {
      needed.push_back(it->second);
      needed_bytes += it->second.size;
    }
  }

  std::cout << "Server requires " << needed.size() << " files, total " << human_bytes(needed_bytes) << '\n';
  if (needed.empty()) {
    return SyncStats{0, 0, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
  }

  std::cout << "Uploading with " << std::min<std::size_t>(config.max_connections, needed.size())
            << " connections, mode " << to_string(config.transfer_mode) << "...\n";

  std::queue<FileInfo> queue;
  for (const auto& file : needed) {
    queue.push(file);
  }

  std::mutex queue_mutex;
  std::mutex cout_mutex;
  std::atomic<std::uint64_t> uploaded_bytes{0};
  std::atomic<std::size_t> uploaded_files{0};
  std::atomic<bool> failed{false};
  std::string first_error;
  std::mutex error_mutex;

  auto worker = [&] {
    while (!failed.load()) {
      std::optional<FileInfo> next;
      {
        std::lock_guard lock(queue_mutex);
        if (queue.empty()) {
          return;
        }
        next = queue.front();
        queue.pop();
      }

      try {
        upload_one_file(config, response.session_id, *next, uploaded_bytes, uploaded_files, cout_mutex);
      } catch (const std::exception& e) {
        failed.store(true);
        std::lock_guard lock(error_mutex);
        if (first_error.empty()) {
          first_error = e.what();
        }
      }
    }
  };

  std::vector<std::thread> threads;
  const auto thread_count = std::min<std::size_t>(config.max_connections, needed.size());
  for (std::size_t i = 0; i < thread_count; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& thread : threads) {
    thread.join();
  }

  if (failed.load()) {
    throw std::runtime_error(first_error);
  }

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return SyncStats{uploaded_files.load(), uploaded_bytes.load(), elapsed};
}

void print_status(const ClientConfig& config, const std::optional<SyncStats>& stats) {
  std::cout << "Client ID: " << config.client_id << '\n'
            << "Directory: " << fs::absolute(config.directory).string() << '\n'
            << "Server: " << config.server_host << ':' << config.server_port << '\n'
            << "Mode: " << to_string(config.transfer_mode) << '\n'
            << "Connections: " << config.max_connections << '\n';
  if (stats) {
    const auto mbps = stats->elapsed_seconds > 0.0
                          ? (static_cast<double>(stats->bytes_uploaded) * 8.0 / 1000.0 / 1000.0 /
                             stats->elapsed_seconds)
                          : 0.0;
    std::cout << "Last sync: " << stats->files_uploaded << " files, "
              << human_bytes(stats->bytes_uploaded) << ", ";
    std::cout.setf(std::ios::fixed);
    std::cout.precision(2);
    std::cout << stats->elapsed_seconds << "s, " << mbps << " Mbps\n";
  }
}

void print_files(const ClientConfig& config) {
  const auto files = scan_directory(config.directory);
  for (const auto& file : files) {
    std::cout << file.name << "  " << human_bytes(file.size) << "  " << file.sha256 << '\n';
  }
}

void run_cli(ClientConfig config) {
  std::optional<SyncStats> last_stats;
  std::cout << "MyDrive Client\n";
  print_status(config, last_stats);
  std::cout << "\nCommands: sync, status, files, mode buffered|sendfile, connections 1..32, exit\n";

  std::string line;
  while (std::cout << "\n> " && std::getline(std::cin, line)) {
    try {
      std::istringstream input(line);
      std::string command;
      input >> command;

      if (command == "sync") {
        last_stats = synchronize(config);
        std::cout << "Sync completed. Uploaded: " << human_bytes(last_stats->bytes_uploaded)
                  << ", elapsed: ";
        std::cout.setf(std::ios::fixed);
        std::cout.precision(2);
        std::cout << last_stats->elapsed_seconds << "s\n";
      } else if (command == "status") {
        print_status(config, last_stats);
      } else if (command == "files") {
        print_files(config);
      } else if (command == "mode") {
        std::string value;
        input >> value;
        config.transfer_mode = parse_transfer_mode(value);
        std::cout << "Mode: " << to_string(config.transfer_mode) << '\n';
      } else if (command == "connections") {
        unsigned int value = 0;
        input >> value;
        if (value < 1 || value > 32) {
          throw std::runtime_error("connections must be in range 1..32");
        }
        config.max_connections = value;
        std::cout << "Connections: " << config.max_connections << '\n';
      } else if (command == "exit" || command == "quit") {
        return;
      } else if (command.empty()) {
        continue;
      } else {
        std::cout << "Unknown command\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "error: " << e.what() << '\n';
    }
  }
}

}  // namespace
}  // namespace mydrive

int main(int argc, char** argv) {
  try {
    auto config = mydrive::read_config(argc, argv);
    if (mydrive::has_arg(argc, argv, "--sync-once")) {
      const auto stats = mydrive::synchronize(config);
      std::cout << "Sync completed. Uploaded: " << mydrive::human_bytes(stats.bytes_uploaded)
                << ", elapsed: ";
      std::cout.setf(std::ios::fixed);
      std::cout.precision(2);
      std::cout << stats.elapsed_seconds << "s\n";
    } else {
      mydrive::run_cli(config);
    }
  } catch (const std::exception& e) {
    std::cerr << "client error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
