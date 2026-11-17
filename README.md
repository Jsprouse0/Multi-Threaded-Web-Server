# Event-Driven HTTP/1.1 Server (Linux, C++20)

A learning project HTTP/1.1 server built from a basic BSD sockets starter. A **single epoll reactor** owns socket I/O, while a **worker thread pool** processes requests using a **bounded lock-free MPMC queue**. Workers notify the reactor with `eventfd`. Designed for reproducible Linux benchmarking with `wrk`.

> **Scope:** This is a deliberately small educational HTTP server, **not** a hardened public-facing HTTP implementation or TLS reverse proxy. Use it on a trusted development network. Request uploads/chunked encoding, HTTP/2, TLS, websocket upgrades, and comprehensive HTTP security validation are not implemented.

## Layout

```text
.
├── CMakeLists.txt
├── include/
│   ├── mpmc_queue.hpp       # bounded Vyukov-style MPMC ring
│   └── server.hpp           # connection and server declarations
├── src/
│   ├── main.cpp             # CLI / lifecycle
│   └── server.cpp           # epoll, HTTP parsing, request dispatch, socket I/O
├── tests/
│   └── mpmc_test.cpp        # multi-producer/multi-consumer exactly-once test
└── bench/
    ├── benchmark.sh        # wrk output capture
    ├── percentiles.lua     # exact p50/p95/p99 latency extraction
    └── context-switches.sh # sum context switches across all server threads
```

## Requirements

- **Linux** (native or WSL2) with epoll, eventfd, `/proc` (kernel 5.x+ recommended)
- GCC 11+ or Clang 14+ with C++20 standard library (`std::counting_semaphore`)
- CMake 3.20+, make/ninja, pthread support
- `curl`, `wrk` (benchmark only), `sysstat` (optional: `pidstat`)
- CLion 2026.2.3: select a **Linux toolchain**; native Windows MSVC/MinGW does not provide Linux `epoll` or `eventfd`.

Ubuntu/WSL2:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build curl wrk sysstat
```

## Build and run

From project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/epoll_http 8080 4
```

Optional positional arguments: `./build/epoll_http [port] [worker_threads]`. Defaults are port `8080` and `hardware_concurrency` workers (minimum 1). Maximum 256 workers. Bind address is `0.0.0.0` (all interfaces); **only run in a trusted environment**.

Try another terminal:

```bash
curl -v http://127.0.0.1:8080/
curl -v http://127.0.0.1:8080/health
curl -I http://127.0.0.1:8080/health
curl -v http://127.0.0.1:8080/nope
```

Stop with `Ctrl+C`. Console prints total completed requests and overloads on shutdown.

### CLion 2026.2.3

1. **File → Open** and choose the directory containing `CMakeLists.txt` (not an individual `.cpp`).
2. **Settings → Build, Execution, Deployment → Toolchains**: choose your **Linux** compiler/toolchain. On Windows use WSL2 toolchain; on native Linux choose System.
3. **Settings → Build, Execution, Deployment → CMake**: use a **Release** profile for benchmarking (Debug is useful while learning).
4. Select the `epoll_http` run target. Set **Program arguments** to `8080 4` if desired, then Run.
5. For reproducible benchmark timing, benchmark the compiled Release binary from a terminal rather than under the debugger.

## Architecture

```text
 TCP clients
     │
     ▼
 nonblocking listen/accept4
     │
     ▼
 main thread: epoll_wait ──────────── eventfd ◀────────┐
     │                                                   │
 recv / incremental HTTP header parse                    │
     │                                                   │
     ▼                                                   │
 bounded lock-free MPMC jobs queue                       │
     │                                                   │
     ├── worker 1 ── handle(request) ──┐                 │
     ├── worker 2 ── handle(request) ──┼── mutex deque ──┤
     └── worker N ── handle(request) ──┘  of results     │
                                                         │
 main thread drains completions and nonblocking send ◀──┘
```

- **Reactor thread** calls `epoll_wait()` and manages socket lifetime, parser buffers, output buffers, and state. Epoll uses **level-triggered** readiness. `EPOLLIN` is temporarily disabled while one request is in flight, and `EPOLLOUT` is enabled only when output is pending. Avoiding continuous `EPOLLOUT` subscription prevents busy loops.
- **Jobs** carry a monotonic connection ID, not just an FD. Linux may recycle FD numbers; this prevents late worker completions from being delivered to a different client.
- **MPMC queue** uses per-slot sequence counters and atomics with acquire/release synchronization, CAS on producer/consumer indexes, fixed capacity 4096; `try_push` may fail when full. `std::counting_semaphore` wakes workers without a constant busy-spin. The queue is **lock-free**, but the overall server is **not globally lock-free**: completed responses use a mutex-backed deque, and the semaphore may block workers.
- **Completion channel** workers push results into a protected deque, write 8 bytes to `eventfd`, and the epoll thread drains and sends them.
- **Backpressure**: queue saturation returns `503 Service Unavailable`; each socket permits one job in flight. Connection count is capped at 20,000; request/header buffer is capped at 16 KiB.
- **HTTP**: `GET` and `HEAD` work with `/` and `/health`; other paths `404`; other methods `405`; HTTP/1.1 requires `Host`; `Connection: close` supported. Request bodies and transfer encodings are unsupported. The parser is intentionally narrow and unsuitable for untrusted public traffic.

## Benchmark: `wrk`

Start server Release build, then from another terminal:

```bash
./bench/benchmark.sh http://127.0.0.1:8080/ 4 200 30s
```

The runner invokes `wrk -t4 -c200 -d30s --latency -s bench/percentiles.lua URL`, writes a dated text report under `benchmarks/`, and prints:

- **Requests/sec**: completed responses / wall-clock seconds
- **Latency p50/p95/p99**, in milliseconds: obtained directly from wrk's sampled latency distribution via `latency:percentile()` (the default `wrk --latency` table does **not** include p95)
- Standard wrk summary: average latency, stdev, transfer rate, errors/timeouts

**Benchmark values are not prefilled** because they depend on hardware, Linux kernel, CPU governor, kernel limits, concurrency, and load. `wrk` is not installed in the provided build environment, so this package does not claim measured throughput or percentiles.

Recommended repeatable runs (avoid running all simultaneously):

```bash
./bench/benchmark.sh http://127.0.0.1:8080/ 2 50 30s
./bench/benchmark.sh http://127.0.0.1:8080/ 4 200 30s
./bench/benchmark.sh http://127.0.0.1:8080/ 8 1000 30s
```

Run once to warm up; then run each configuration 3–5 times and get a report on median throughput and p99. 
### Context-switch study

Find the PID in the server terminal (or `pgrep -f './build/epoll_http'`), then:

```bash
./bench/context-switches.sh <SERVER_PID> 30
# Run wrk in parallel for the same 30-second window.
pidstat -w -t -p <SERVER_PID> 1
```

`context-switches.sh` adds `/proc/PID/task/TID/status` voluntary + nonvoluntary context-switch counters across **all** current threads, before and after the sampling interval. The script reports a total delta and switches per second. It is approximate if threads are created/removed during sampling. Workers in this project are long-lived. `pidstat -w -t` separates voluntary (`cswch/s`) and involuntary (`nvcswch/s`) per thread. It helps see whether increasing thread counts causes more scheduler overhead without proportional throughput gains. Compare **switches/request** = `context_switch_delta / completed_requests_in_same_window`


| workers | wrk threads | connections | RPS | p50 ms | p95 ms | p99 ms | total ctx switches/sec |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 2 | 50 | TBD | TBD | TBD | TBD | TBD |
| 4 | 4 | 200 | TBD | TBD | TBD | TBD | TBD |
| 8 | 8 | 1000 | TBD | TBD | TBD | TBD | TBD |

For baseline context switching, sample idle server for 30s; repeat under the same load on each test. Monitor CPU saturation (`top -H -p PID`, `pidstat -u -t`) and file descriptors (`ls /proc/PID/fd | wc -l`). Increase open-file limit for 1000+ connections if necessary: `ulimit -n 65535`.

## Verify functionality / troubleshoot

```bash
ctest --test-dir build --output-on-failure
curl --http1.1 -v -H 'Connection: close' http://127.0.0.1:8080/
ss -ltnp | grep 8080
```

- `Address already in use`: stop other service or run `./build/epoll_http 8081 4`.
- `Permission denied`: use ports >= 1024.
- CLion compilation fails on `sys/epoll.h`: you're using a Windows toolchain, not WSL/Linux.
- Throughput plateaus: check server and wrk client CPU contention, connection count, queue contention, and file-descriptor limits.
- If p99 balloons: first reduce concurrency, then compare worker count and context switches. Lock-free coordination doesn't guarantee lower tail latency.
