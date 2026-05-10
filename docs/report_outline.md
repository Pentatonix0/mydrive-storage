# HW4 Report Outline

## Goal

Implement a multi-user TCP file synchronization service similar to a minimal cloud drive.

## Claimed Grade

Claim: up to 10 points.

Stack: C++20 + Boost.Asio, without C++ coroutines.

Implemented advanced parts:

- Parallel upload through several TCP connections.
- DMA/zero-copy transfer mode through `sendfile`.
- Buffered mode for comparison.

## Architecture

Client:

- Reads JSON config.
- Scans configured directory.
- Computes SHA-256 for each regular file.
- Sends manifest to the server.
- Uploads missing/changed files with up to 32 parallel TCP connections.

Server:

- Runs Boost.Asio async accept loop.
- Creates a session per TCP connection.
- Reads binary protocol frames with `async_read`.
- Stores files under `storage_root/client_id`.
- Verifies SHA-256 and atomically renames temp files.

## Experiments

Record measurements for:

- 50 MB file, buffered.
- 50 MB file, sendfile.
- 100 MB file, buffered.
- 100 MB file, sendfile.
- 250 MB file, buffered.
- 250 MB file, sendfile.

For parallel upload, use 1, 4, 8, 16 connections and compare total throughput.

## Network Observations

Collect:

- Wireshark screenshot with manifest exchange.
- Wireshark screenshot with several parallel TCP streams.
- MSS value.
- Window size / scaling.
- Slow start behavior.
- Retransmissions or congestion, if observed.

Useful commands:

```bash
ss -tin sport = :9000
sudo tcpdump -i any port 9000 -w mydrive.pcap
tc qdisc show
```

