# MyDrive

TCP file synchronization project for HW4. The implementation uses C++20 and Boost.Asio without C++ coroutines.

## Features

- Custom binary protocol over TCP with explicit byte serialization.
- Async server based on `boost::asio::async_read` / `async_write`.
- Client manifest exchange before file upload.
- Per-user server storage under `storage_root/client_id`.
- One-way synchronization from client to server. Files that exist only on the server are ignored and are not downloaded to the client.
- Parallel uploads through 1-32 independent TCP connections.
- Two transfer modes:
  - `buffered`: userspace read/write chunks.
  - `sendfile`: OS zero-copy/DMA path (`sendfile` on Linux/macOS).
- SHA-256 verification after every upload.
- Temporary `.part` files with atomic rename after checksum success.

## Build

Dependencies:

- C++20 compiler
- CMake 3.22+
- Boost.Asio headers
- OpenSSL

```bash
cmake -S . -B build
cmake --build build -j 4
```

## Server Usage

Edit `config/server.json` if needed:

```json
{
  "listen_host": "0.0.0.0",
  "listen_port": 9000,
  "storage_root": "./server_storage",
  "threads": 4
}
```

Start the server:

```bash
./build/mydrive_server --config config/server.json
```

The server accepts an unlimited number of clients/connections from the application logic point of view. Effective limits are left to the OS and Boost.Asio.

## Client Usage

Edit `config/client.json`:

```json
{
  "client_id": "lebedev-local",
  "server_host": "127.0.0.1",
  "server_port": 9000,
  "directory": "./client_files",
  "max_connections": 8,
  "transfer_mode": "sendfile"
}
```

Start interactive CLI:

```bash
./build/mydrive_client --config config/client.json
```

Commands:

```text
help
sync
status
files
mode buffered
mode sendfile
connections 1
connections 8
connections 32
exit
```

`help` prints command descriptions in Russian.

Run one synchronization round and exit:

```bash
./build/mydrive_client --config config/client.json --mode buffered --sync-once
./build/mydrive_client --config config/client.json --mode sendfile --sync-once
```

Synchronization is intentionally one-way. The client sends its local manifest, the server asks only for missing or changed client files, and uploads go only from the client to the server. If a file is added manually on the server, the client does not download it and does not treat it as part of the next synchronization round.

## Protocol

Every control message is sent as:

```text
uint32 magic        // MYDR
uint16 version      // 1
uint16 message_type
uint32 payload_size
payload bytes
```

The receiver first reads the fixed-size header, then reads exactly `payload_size` bytes, then deserializes the payload. This is implemented explicitly instead of relying on high-level message endpoints.

Message types:

```text
MANIFEST_REQUEST
MANIFEST_RESPONSE
UPLOAD_BEGIN
UPLOAD_READY
UPLOAD_FINISHED
ERROR
```

File content is not serialized as an object. The client first sends `UPLOAD_BEGIN`, waits for `UPLOAD_READY`, then streams exactly the declared number of file bytes over the same TCP connection. In `sendfile` mode those bytes are sent through the OS zero-copy path.

## Demo Data

Use the helper scripts to generate benchmark files:

```bash
./generate_files.sh client_files
./generate_big_file.sh client_files
./generate_random.sh --n 10 --dir client_files
```

`generate_files.sh` creates:

```text
file_50mb.bin
file_150mb.bin
file_200mb.bin
```

`generate_big_file.sh` creates a large 1500 MB file:

```text
file_1500mb.bin
```

`generate_random.sh --n N` creates `N` files with random sizes from 105 MB to 205 MB:

```bash
./generate_random.sh --n 10
./generate_random.sh --n 10 --dir client_files
```

For the assignment demonstration, also prepare at least 10 files of at least 100 MB each:

```bash
mkdir -p client_files
dd if=/dev/urandom of=client_files/file_01.bin bs=1m count=100
dd if=/dev/urandom of=client_files/file_02.bin bs=1m count=100
```

Repeat until you have the required dataset.

## Report Checklist

Include in the report:

- Selected stack: C++20 + Boost.Asio, no coroutines.
- Architecture: server sessions, client sync controller, upload workers, storage.
- Protocol fragments: frame header, manifest, upload begin, upload finished.
- Screenshots from Wireshark showing TCP streams and several parallel upload connections.
- File list and sizes used in the experiment.
- `buffered` vs `sendfile` measurements for 50 MB, 100 MB, 250 MB.
- Observed MSS, TCP Window, slow start, congestion effects if present.
- Useful commands: `ss`, `tcpdump`, `tc`, `sha256sum`/`shasum`.
