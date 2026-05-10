#include "common/checksum.hpp"
#include "common/config.hpp"
#include "common/protocol.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using boost::asio::ip::tcp;

namespace mydrive {
namespace {

struct ServerConfig {
  std::string listen_host;
  unsigned int listen_port;
  fs::path storage_root;
  unsigned int threads;
};

std::string sanitize_component(const std::string& value) {
  const fs::path path(value);
  const auto filename = path.filename().string();
  if (filename.empty() || filename == "." || filename == ".." || filename != value) {
    throw std::runtime_error("unsafe path component: " + value);
  }
  return filename;
}

std::string make_session_id(const std::string& client_id) {
  static std::atomic<std::uint64_t> counter{1};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return client_id + "-" + std::to_string(now) + "-" + std::to_string(counter.fetch_add(1));
}

class Storage {
 public:
  explicit Storage(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
  }

  ManifestResponse compare_manifest(const ManifestRequest& request) {
    const auto client_dir = directory_for(request.client_id);
    fs::create_directories(client_dir);

    ManifestResponse response;
    response.session_id = make_session_id(request.client_id);

    for (const auto& file : request.files) {
      const auto name = sanitize_component(file.name);
      const auto server_path = client_dir / name;
      bool needed = !fs::exists(server_path) || !fs::is_regular_file(server_path);
      if (!needed && fs::file_size(server_path) != file.size) {
        needed = true;
      }
      if (!needed) {
        needed = sha256_file(server_path) != file.sha256;
      }
      if (needed) {
        response.needed_files.push_back(name);
      }
    }

    return response;
  }

  fs::path temp_path_for(const UploadBegin& upload) {
    const auto dir = directory_for(upload.client_id);
    fs::create_directories(dir);
    return dir / ("." + sanitize_component(upload.filename) + "." + upload.session_id + ".part");
  }

  fs::path final_path_for(const UploadBegin& upload) {
    const auto dir = directory_for(upload.client_id);
    fs::create_directories(dir);
    return dir / sanitize_component(upload.filename);
  }

 private:
  fs::path directory_for(const std::string& client_id) const {
    return root_ / sanitize_component(client_id);
  }

  fs::path root_;
};

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket, Storage& storage)
      : socket_(std::move(socket)), storage_(storage) {}

  ~Session() {
    cleanup_incomplete_upload();
  }

  void start() {
    try {
      std::cout << "[session] connected from " << socket_.remote_endpoint() << '\n';
    } catch (...) {
      std::cout << "[session] connected\n";
    }
    read_header();
  }

 private:
  void read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(header_),
        [this, self](boost::system::error_code ec, std::size_t) {
          if (ec) {
            return;
          }
          try {
            frame_ = decode_header(std::span<const std::uint8_t, kFrameHeaderSize>(header_));
          } catch (const std::exception& e) {
            write_error_and_close(e.what());
            return;
          }
          read_payload();
        });
  }

  void read_payload() {
    if (frame_.payload.empty()) {
      handle_frame();
      return;
    }

    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(frame_.payload),
        [this, self](boost::system::error_code ec, std::size_t) {
          if (ec) {
            return;
          }
          handle_frame();
        });
  }

  void handle_frame() {
    try {
      switch (frame_.type) {
        case MessageType::ManifestRequest:
          handle_manifest_request();
          break;
        case MessageType::UploadBegin:
          handle_upload_begin();
          break;
        default:
          write_error_and_close("unexpected message type");
          break;
      }
    } catch (const std::exception& e) {
      write_error_and_close(e.what());
    }
  }

  void handle_manifest_request() {
    const auto request = parse_manifest_request(frame_.payload);
    std::cout << "[client " << request.client_id << "] manifest: " << request.files.size()
              << " files\n";

    auto response = storage_.compare_manifest(request);
    std::cout << "[client " << request.client_id << "] required: "
              << response.needed_files.size() << " files\n";

    write_frame(MessageType::ManifestResponse, serialize(response), [this] { read_header(); });
  }

  void handle_upload_begin() {
    upload_ = parse_upload_begin(frame_.payload);
    remaining_ = upload_.size;
    received_ = 0;
    temp_path_ = storage_.temp_path_for(upload_);
    output_ = std::make_unique<std::ofstream>(temp_path_, std::ios::binary | std::ios::trunc);
    if (!*output_) {
      throw std::runtime_error("cannot create temp file: " + temp_path_.string());
    }

    std::cout << "[client " << upload_.client_id << "] receiving " << upload_.filename << ", "
              << upload_.size << " bytes, " << to_string(upload_.mode) << '\n';

    write_frame(MessageType::UploadReady, {}, [this] { receive_file_chunk(); });
  }

  void receive_file_chunk() {
    if (remaining_ == 0) {
      finish_upload();
      return;
    }

    const auto to_read = std::min<std::uint64_t>(buffer_.size(), remaining_);
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(buffer_.data(), static_cast<std::size_t>(to_read)),
        [this, self](boost::system::error_code ec, std::size_t n) {
          if (ec) {
            std::cerr << "[upload] receive error: " << ec.message() << '\n';
            cleanup_incomplete_upload();
            return;
          }
          output_->write(buffer_.data(), static_cast<std::streamsize>(n));
          if (!*output_) {
            write_error_and_close("cannot write received file");
            return;
          }
          remaining_ -= n;
          received_ += n;
          receive_file_chunk();
        });
  }

  void finish_upload() {
    output_->close();

    UploadFinished result;
    result.filename = upload_.filename;
    result.bytes = received_;

    try {
      const auto actual_hash = sha256_file(temp_path_);
      if (actual_hash != upload_.sha256) {
        fs::remove(temp_path_);
        temp_path_.clear();
        result.ok = false;
        result.message = "checksum mismatch";
      } else {
        const auto final_path = storage_.final_path_for(upload_);
        fs::rename(temp_path_, final_path);
        temp_path_.clear();
        result.ok = true;
        result.message = "ok";
      }
    } catch (const std::exception& e) {
      result.ok = false;
      result.message = e.what();
    }

    std::cout << "[client " << upload_.client_id << "] " << result.message << ": "
              << upload_.filename << " (" << result.bytes << " bytes)\n";

    write_frame(MessageType::UploadFinished, serialize(result), [this] { read_header(); });
  }

  template <class Next>
  void write_frame(MessageType type, const std::vector<std::uint8_t>& payload, Next next) {
    outbound_ = make_frame(type, payload);
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(outbound_),
        [self, next](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            return;
          }
          next();
        });
  }

  void write_error_and_close(const std::string& message) {
    std::cerr << "[session] error: " << message << '\n';
    outbound_ = make_frame(MessageType::Error, serialize_error(message));
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(outbound_),
        [this, self](boost::system::error_code, std::size_t) {
          boost::system::error_code ignored;
          socket_.shutdown(tcp::socket::shutdown_both, ignored);
          socket_.close(ignored);
        });
  }

  void cleanup_incomplete_upload() {
    if (output_) {
      output_->close();
      output_.reset();
    }
    if (!temp_path_.empty()) {
      std::error_code fs_ignored;
      fs::remove(temp_path_, fs_ignored);
      temp_path_.clear();
    }
  }

  tcp::socket socket_;
  Storage& storage_;
  std::array<std::uint8_t, kFrameHeaderSize> header_{};
  Frame frame_{MessageType::Error, {}};
  std::vector<std::uint8_t> outbound_;
  UploadBegin upload_;
  fs::path temp_path_;
  std::unique_ptr<std::ofstream> output_;
  std::array<char, 1024 * 1024> buffer_{};
  std::uint64_t remaining_ = 0;
  std::uint64_t received_ = 0;
};

class Server {
 public:
  Server(boost::asio::io_context& io, const ServerConfig& config)
      : io_(io),
        storage_(config.storage_root),
        acceptor_(io) {
    const auto address = boost::asio::ip::make_address(config.listen_host);
    tcp::endpoint endpoint(address, static_cast<unsigned short>(config.listen_port));
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(boost::asio::socket_base::max_listen_connections);
  }

  void start() {
    accept();
  }

 private:
  void accept() {
    acceptor_.async_accept(
        boost::asio::make_strand(io_),
        [this](boost::system::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<Session>(std::move(socket), storage_)->start();
          } else {
            std::cerr << "[accept] " << ec.message() << '\n';
          }
          accept();
        });
  }

  boost::asio::io_context& io_;
  Storage storage_;
  tcp::acceptor acceptor_;
};

ServerConfig read_config(const fs::path& path) {
  JsonConfig config(path);
  ServerConfig result;
  result.listen_host = config.string_value("listen_host", "0.0.0.0");
  result.listen_port = config.uint_value("listen_port", 9000);
  result.storage_root = config.string_value("storage_root", "./server_storage");
  result.threads = std::max(1u, config.uint_value("threads", std::thread::hardware_concurrency()));
  return result;
}

fs::path config_path_from_args(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--config") {
      return argv[i + 1];
    }
  }
  return "config/server.json";
}

}  // namespace
}  // namespace mydrive

int main(int argc, char** argv) {
  try {
    const auto config = mydrive::read_config(mydrive::config_path_from_args(argc, argv));
    boost::asio::io_context io;
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { io.stop(); });

    mydrive::Server server(io, config);
    server.start();

    std::cout << "MyDrive Server\n"
              << "Listening on " << config.listen_host << ':' << config.listen_port << '\n'
              << "Storage: " << fs::absolute(config.storage_root).string() << '\n'
              << "Threads: " << config.threads << "\n\n";

    std::vector<std::thread> threads;
    for (unsigned int i = 1; i < config.threads; ++i) {
      threads.emplace_back([&] { io.run(); });
    }
    io.run();
    for (auto& thread : threads) {
      thread.join();
    }
  } catch (const std::exception& e) {
    std::cerr << "server error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
