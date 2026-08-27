# JobU Phase 2 Code-Level Design

## 1. Status and purpose

This document defines Phase 2 of JobU: bounded local IPC, a transport-independent JSON-RPC 2.0 layer, and the first executable `system.info` round trip.

It is based on:

- the JobU v1 technical plan .codex/jobu-v1-technical-plan.md;
- Phase 0 merged as commit `4e6724834dc49df447f9f9224c88a7e03acab207`;
- Phase 1 merged as commit `f2a438155a3be04a20333563a4119e0baa0313c3`;
- current `main` at commit `1652407584742c29de2c0f0865c352124d029b3b`;
- the existing `jb::core::Application`, `EventLoop`, `Object`, `Signal`, and `IODevice` contracts;
- the existing `jb::net::TcpSocket` implementation as a style and event-loop integration reference;
- the repository's mandatory staged implementation workflow.

Phase 2 remains below the JobU management and scheduling layers. It establishes a reusable byte-stream RPC implementation and proves it through one read-only application method. It does not add the JobU schema, queue/job repositories, scheduler, runners, or daemon configuration.

## 2. Scope and exit criteria

Phase 2 delivers:

1. A generic close notification on `jb::core::IODevice`.
2. Project-owned JSON values and JSON encoding/decoding with nlohmann/json kept private.
3. Incremental, bounded LSP-style stream framing.
4. Strict JSON-RPC 2.0 request, notification, batch, response, and error processing.
5. A synchronous-on-the-event-loop method dispatcher.
6. A transport-independent RPC server over owned `IODevice` connections.
7. A transport-independent RPC client with multiple outstanding requests and ID correlation.
8. Linux `LocalSocket` and `LocalServer` implementations.
9. A minimal `system.info` method and `jobuctl system info` flow over a filesystem Unix-domain socket.
10. macOS `LocalSocket` and `LocalServer` implementations, added only after the complete Linux path works.
11. Focused unit, Linux integration, Linux executable, macOS integration, and public-boundary tests.

Phase 2 exit criteria are:

- `jobuctl --socket <path> system info` communicates with a foreground `jobud` over a Unix-domain stream socket;
- the response reports daemon version, API major/minor, and capabilities;
- multiple requests may be outstanding on one persistent RPC connection and are correlated by ID;
- fragmented and coalesced frames are parsed deterministically within configured limits;
- malformed framing closes only the offending connection;
- malformed JSON and invalid JSON-RPC envelopes produce the standard JSON-RPC errors when a valid frame boundary exists;
- nlohmann/json types and headers appear in no public header;
- local socket system headers, descriptors, and platform credential structures appear in no public header;
- the Linux implementation and tests are completed before any macOS implementation stage begins;
- Linux and macOS use separate backend source files selected by CMake;
- every new public header and every public declaration added in Phase 2 has useful Doxygen documentation;
- all existing Phase 0 and Phase 1 tests continue to pass.

## 3. Explicitly deferred

The following are not Phase 2 work:

- the JobU database schema, application migrations, and repositories;
- queue, job, run, attempt, secret, or scheduler RPC methods;
- authentication, authorization, RBAC, JWT, TCP, TLS, or WebSocket transports;
- remote hostname resolution;
- asynchronous RPC handler completion or a worker pool;
- streaming results, subscriptions, and server-initiated requests;
- JSON schema generation or reflection-based DTO serialization;
- request cancellation on the wire; local client correlation cancellation is included;
- daemon INI configuration and final socket owner/group management;
- stale socket recovery policy for production startup;
- graceful `SIGINT`/`SIGTERM` daemon shutdown;
- machine-readable `jobuctl` output modes beyond the protocol itself;
- Windows local IPC;
- HTTP and CLI job execution.

Phase 2 must not introduce abstractions for these deferred features beyond the small operation context and transport boundary described below.

## 4. Architecture and dependency boundaries

```text
jobuctl                                                jobud
   |                                                     |
   v                                                     v
jb::rpc::Client <--- LSP-framed JSON-RPC ---> jb::rpc::Server
   |                                                     |
   v                                                     v
jb::net::LocalSocket                           jb::net::LocalServer
                                                         |
                                                         v
                                                accepted LocalSocket
```

The layers have these responsibilities:

| Layer | Responsibility | Must not know about |
| --- | --- | --- |
| `jb::core::IODevice` | Common byte-stream lifecycle and signals | JSON, RPC, local sockets |
| `jb::net` local IPC | Nonblocking filesystem Unix sockets, buffering, peer credentials | JSON-RPC methods or JobU DTOs |
| JSON codec | Project-owned JSON tree conversion | Framing, sockets, JobU methods |
| stream framing | `Content-Length` headers and byte limits | JSON syntax or RPC envelopes |
| RPC protocol | JSON-RPC envelope validation and encoding | Unix paths or file descriptors |
| RPC client/server | correlation, dispatch, connection lifetime and limits | concrete socket classes |
| `jobu`/executables | `system.info` DTO and application composition | native socket or nlohmann/json types |

The RPC target depends publicly on `core` and privately on nlohmann/json. It does not depend on `net`. The `net` target depends only on `core`. The application composes the two by passing accepted `LocalSocket` objects to `jb::rpc::Server` as `std::unique_ptr<jb::core::IODevice>`.

Handlers run synchronously on the owning event-loop thread. A handler must not block, sleep, wait for network I/O, or start nested event processing. This is appropriate for the short in-memory and database operations planned for v1. If a future method needs asynchronous completion, that will be an explicit API extension rather than hidden blocking.

## 5. Targets, dependencies, and source layout

### 5.1 CMake targets

Keep the existing targets and dependency direction:

```text
core
  ^
  |---- net
  |---- rpc ----(PRIVATE)---- nlohmann_json::nlohmann_json
           ^
           |
          jobu
         /    \
     jobud   jobuctl
```

`src/rpc/CMakeLists.txt` adds:

```cmake
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(rpc
    PUBLIC core
    PRIVATE nlohmann_json::nlohmann_json
)
```

No nlohmann/json type may occur in a target's public compile interface. The dependency is an implementation choice and may later be replaced without changing a public header.

The final `src/net/CMakeLists.txt` selects local IPC sources explicitly:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SRCS
        local_socket_linux.cpp
        local_server_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    list(APPEND SRCS
        local_socket_macos.cpp
        local_server_macos.cpp
    )
endif()
```

Do not create one source file containing interleaved Linux/macOS `#ifdef` branches. Small shared helpers may be extracted only after both implementations prove that they are genuinely identical.

### 5.2 Planned files

```text
src/core/
  io_device.cpp                         # add generic closed emission support
  io_device.hpp

src/net/
  CMakeLists.txt
  local_server.hpp
  local_server_priv.hpp
  local_server_linux.cpp
  local_server_macos.cpp
  local_socket.hpp
  local_socket_priv.hpp
  local_socket_linux.cpp
  local_socket_macos.cpp

src/rpc/
  CMakeLists.txt
  client.cpp
  client.hpp
  client_priv.hpp
  framing.cpp
  framing.hpp
  json.cpp
  json.hpp
  protocol.cpp
  protocol.hpp
  protocol_priv.hpp
  rpc.cpp
  rpc.hpp
  server.cpp
  server.hpp
  server_priv.hpp

src/jobu/
  CMakeLists.txt
  system_info.cpp
  system_info.hpp

src/jobud/
  main.cpp

src/jobuctl/
  main.cpp

test/
  support/
    memory_io_device.cpp
    memory_io_device.hpp
  io-device-test.cpp                  # extend existing test
  json-test.cpp
  rpc-framing-test.cpp
  rpc-protocol-test.cpp
  rpc-server-test.cpp
  rpc-client-test.cpp
  local-socket-test.cpp
  local-server-test.cpp
  local-rpc-test.cpp
  system-info-test.cpp
  jobuctl-system-info-test.cpp
  net-public-headers-test.cpp
  rpc-public-headers-test.cpp
```

Private files may be adjusted during implementation, but the public classes and target boundaries must remain as specified unless Codex stops and requests approval for a design revision.

## 6. Mandatory public documentation and `[[nodiscard]]` policy

### 6.1 Doxygen gate

Every Phase 2 stage that adds or changes a public header must complete its Doxygen documentation in the same stage. Documentation is not a final cleanup stage.

Each new public header must contain:

- an `@file` block describing the header's responsibility;
- a namespace-level type comment for every public class, struct, enum, alias, and constant;
- comments for every public constructor, destructor, method, free function, signal, and public data member;
- `@param` and `@return` where they add information beyond the signature;
- `@throws`, `@warning`, or `@note` for lifetime, ownership, thread-affinity, destructive-close, or exception behavior;
- explicit statements for borrowed versus owned objects and required lifetime ordering;
- descriptions of units and limits for byte counts, IDs, modes, and timeouts.

Comments must explain the contract and rationale, not merely restate the name. Public-header tests must include every new header directly. Before Codex reports a stage complete, its report must list the public headers changed and confirm that their public declarations were reviewed for Doxygen coverage.

### 6.2 `[[nodiscard]]`

Continue the Phase 1 selective policy.

Use `[[nodiscard]]` for:

- parsing, encoding, `listen()`, connection attachment, and RPC calls where runtime failure must normally be handled;
- getters whose sole purpose is their returned value;
- operations returning received data or a pending connection.

Do not add `[[nodiscard]]` merely because a method returns `bool` or `Result`. In particular, omit it from methods that normally succeed and whose failure mainly indicates programmer misuse, such as duplicate method registration/removal. Cleanup methods are `void` where practical.

The `jb::core::Result` type itself is already `[[nodiscard]]`; do not duplicate attributes mechanically on every Result-returning declaration.

## 7. Generic `IODevice` close lifecycle

RPC needs to fail outstanding requests and release server connection state when an arbitrary byte stream closes. `IODevice` currently exposes read, write, and error signals but no generic close notification.

Extend `io_device.hpp` with:

```cpp
namespace jb::core {

class IODevice : public Object {
public:
    // Existing API remains unchanged.

    /// Emitted once when an open device transitions to closed.
    Signal<> closed;

protected:
    /// Emits closed after a concrete device has completed an open-to-closed transition.
    void emit_closed();
};

} // namespace jb::core
```

Rules:

- an already closed device does not emit again when `close()` is called;
- both explicit close and peer/error-driven close emit exactly once for each open lifecycle;
- any final readable bytes are made available and `ready_read` is emitted before `closed`;
- `TcpSocket::disconnected` remains for source compatibility and is emitted in addition to `IODevice::closed`;
- `File` emits `closed` after a successfully open file is closed;
- destructors do not emit signals;
- adding the signal must not otherwise change TCP or file behavior.

The server/client connect to this base signal and therefore do not downcast to a transport.

## 8. Local IPC public API

### 8.1 Common value types

Add `local_socket.hpp` with project-owned, platform-neutral values:

```cpp
namespace jb::net {

enum class LocalSocketState : std::uint8_t {
    Unconnected,
    Connecting,
    Connected,
    Closing,
};

struct LocalPeerCredentials {
    std::optional<std::uint64_t> process_id;
    std::optional<std::uint64_t> user_id;
    std::optional<std::uint64_t> group_id;
};

} // namespace jb::net
```

Optional fields make the public contract honest across platforms. Linux fills all three fields from `SO_PEERCRED`. macOS fills effective user and group IDs through `getpeereid()`; `process_id` remains absent unless a reliable supported API is deliberately added later.

Native `ucred`, `xucred`, `pid_t`, `uid_t`, `gid_t`, `sockaddr_un`, and file descriptor types stay private.

### 8.2 `LocalSocket`

Add a Qt-inspired asynchronous local stream socket:

```cpp
namespace jb::net {

class LocalServer;

class LocalSocket final : public jb::core::IODevice {
public:
    explicit LocalSocket(jb::core::Object* parent = nullptr);
    ~LocalSocket() override;

    void connect_to_server(std::filesystem::path const& path);
    void disconnect_from_server();
    void abort();

    [[nodiscard]] auto state() const noexcept -> LocalSocketState;
    [[nodiscard]] auto server_path() const noexcept -> std::filesystem::path const&;
    [[nodiscard]] auto peer_credentials() const noexcept -> LocalPeerCredentials const&;

    void set_read_buffer_limit(std::size_t bytes);
    [[nodiscard]] auto read_buffer_limit() const noexcept -> std::size_t;

    [[nodiscard]] auto is_open() const -> bool override;
    void close() override;
    [[nodiscard]] auto read(std::size_t max_size) -> std::string override;
    [[nodiscard]] auto read_all() -> std::string override;
    [[nodiscard]] auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string override;
    [[nodiscard]] auto can_read_line() const -> bool override;
    auto write(std::string_view data) -> std::size_t override;
    [[nodiscard]] auto bytes_available() const -> std::size_t override;

    jb::core::Signal<> connected;
    jb::core::Signal<> disconnected;

private:
    friend class LocalServer;
    struct Private;
};

} // namespace jb::net
```

The private accepted-descriptor constructor is accessible only to `LocalServer` and is not part of the public contract.

Semantics:

- `connect_to_server()` is asynchronous and returns errors through `IODevice::error_occurred` and stored error state;
- only filesystem Unix-domain socket paths are accepted; Linux abstract namespace addresses are rejected because they cannot use filesystem permissions as the authorization boundary;
- a new connect attempt aborts any previous connection and clears buffered input/output, errors, path, and credentials;
- accepted sockets begin in `Connected` state and never emit `connected` retroactively;
- graceful disconnect flushes queued writes; `abort()` drops them;
- a read-buffer limit of zero means unlimited, but RPC always configures a finite limit;
- once the input buffer reaches its limit, read watching pauses until the caller drains bytes;
- peer EOF preserves already buffered input, emits `ready_read` when needed, then `closed` and `disconnected`;
- all operations except inherited thread-safe signal connection management are object-thread-only;
- no operation exposes or transfers a native descriptor.

### 8.3 `LocalServer`

Add `local_server.hpp`:

```cpp
namespace jb::net {

struct LocalServerOptions {
    std::filesystem::perms permissions{
        std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write};
    int         backlog{128};
    std::size_t max_pending_connections{64};
    std::size_t accepted_read_buffer_limit{2U * 1024U * 1024U};
};

class LocalServer final : public jb::core::Object {
public:
    explicit LocalServer(jb::core::Object* parent = nullptr);
    ~LocalServer() override;

    [[nodiscard]] auto listen(std::filesystem::path const& path,
                              LocalServerOptions options = {}) -> bool;
    void close();

    [[nodiscard]] auto is_listening() const noexcept -> bool;
    [[nodiscard]] auto server_path() const noexcept -> std::filesystem::path const&;
    [[nodiscard]] auto pending_connection_count() const noexcept -> std::size_t;
    [[nodiscard]] auto take_next_connection() -> std::unique_ptr<LocalSocket>;

    [[nodiscard]] auto error() const noexcept -> jb::core::IOError;
    [[nodiscard]] auto error_string() const noexcept -> std::string const&;

    jb::core::Signal<>                         new_connection;
    jb::core::Signal<jb::core::IOError, std::string> accept_error;

private:
    struct Private;
};

} // namespace jb::net
```

`take_next_connection()` makes ownership explicit instead of returning an ambiguously owned raw pointer. The caller either keeps the `unique_ptr` or transfers it to `jb::rpc::Server`.

Server rules:

- the parent directory must already exist;
- an empty path, overlong native path, invalid option, non-socket existing path, or already occupied path makes `listen()` fail without unlinking anything;
- production stale-socket recovery is deferred; `listen()` never silently deletes a pre-existing path;
- after `bind()`, permissions are applied before `listen()` begins accepting clients;
- ownership changes are deferred to daemon privilege/configuration work;
- the listening and accepted descriptors are nonblocking and close-on-exec;
- accept drains until it would block or the pending limit is reached;
- when the pending queue is full, acceptance pauses and resumes when a connection is taken;
- one `new_connection` emission may represent multiple queued connections; consumers drain with `take_next_connection()`;
- closing drops pending, not-yet-transferred sockets;
- close removes the filesystem entry only if its device/inode identity still matches the entry created by this server instance;
- an accept error is stored, emitted, and does not destroy already accepted connections;
- the class is event-loop-thread-affine.

### 8.4 Linux implementation

Linux uses:

- `AF_UNIX`, `SOCK_STREAM`, `SOCK_NONBLOCK`, and `SOCK_CLOEXEC`;
- `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`;
- `SO_PEERCRED` immediately after connection/accept;
- `MSG_NOSIGNAL` for writes;
- the existing `EventLoop::watch_fd()` interface;
- length calculations based on `offsetof(sockaddr_un, sun_path)` rather than `sizeof(sockaddr_un)` assumptions.

Every syscall handles `EINTR`; nonblocking `EAGAIN`/`EWOULDBLOCK` is not an error. Connection completion checks `SO_ERROR` just as `TcpSocket` does.

### 8.5 macOS implementation, deliberately later

No macOS local-socket implementation is written during the Linux stages. After the Linux RPC and executable path is complete, separate macOS stages implement the same public API using:

- `socket()`/`accept()` followed by `fcntl()` for nonblocking and close-on-exec state;
- `SO_NOSIGPIPE` on connected and accepted sockets;
- `getpeereid()` for effective UID/GID;
- the existing kqueue-backed `EventLoop`;
- the same filesystem path, permission, pending-queue, buffering, and inode-safe cleanup semantics.

Do not weaken the generic API to make macOS easier, and do not copy Linux-only credential promises into the macOS contract.

## 9. Project-owned JSON values and codec

Add `json.hpp`:

```cpp
namespace jb::rpc {

struct JsonNull {
    auto operator==(JsonNull const&) const -> bool = default;
};

struct JsonValue {
    using Array  = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Data   = std::variant<JsonNull,
                                bool,
                                std::int64_t,
                                std::uint64_t,
                                double,
                                std::string,
                                Array,
                                Object>;

    Data data{JsonNull{}};

    auto operator==(JsonValue const&) const -> bool = default;
};

struct JsonLimits {
    std::size_t max_depth{64};
};

[[nodiscard]] auto parse_json(std::string_view text, JsonLimits limits = {})
    -> jb::core::Result<JsonValue, jb::core::Error>;

[[nodiscard]] auto serialize_json(JsonValue const& value)
    -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::rpc
```

Rules:

- the tree owns all strings, arrays, and objects;
- `std::map` gives deterministic object ordering for stable tests and canonical request hashing later;
- signed, unsigned, and floating JSON numbers remain distinct;
- parsing rejects invalid UTF-8, duplicate object member names, excessive nesting, integer overflow, and non-finite numbers;
- serialization rejects non-finite doubles;
- errors use stable `rpc.json.*` codes and contain no input body;
- nlohmann/json is included only by `json.cpp` or a private header;
- conversion is explicit; no implicit constructors are added in Phase 2.

The JSON codec is general-purpose. JSON-RPC-specific object/member validation belongs in the protocol layer.

## 10. LSP-style stream framing

Add `framing.hpp`:

```cpp
namespace jb::rpc {

struct FramingLimits {
    std::size_t max_header_bytes{16U * 1024U};
    std::size_t max_body_bytes{1024U * 1024U};
};

class StreamFramer final {
public:
    explicit StreamFramer(FramingLimits limits = {});

    [[nodiscard]] auto append(std::string_view bytes)
        -> jb::core::Result<std::vector<std::string>, jb::core::Error>;
    void reset() noexcept;

    [[nodiscard]] auto buffered_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto limits() const noexcept -> FramingLimits const&;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

[[nodiscard]] auto frame_message(std::string_view body, FramingLimits limits = {})
    -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::rpc
```

Wire form:

```text
Content-Length: <decimal UTF-8 body byte count>\r\n
\r\n
<body bytes>
```

Parser rules:

- input may contain any fragment of a header/body or several complete messages;
- only CRLF terminators are accepted;
- header names are ASCII case-insensitive;
- exactly one `Content-Length` is required;
- the value contains decimal digits only after optional HTTP-style surrounding whitespace;
- signs, negative values, overflow, conflicting/duplicate lengths, and trailing junk are rejected;
- unknown syntactically valid headers are ignored and count toward the header limit;
- `Content-Type`, if supplied, must specify UTF-8; `utf8` is accepted as the LSP compatibility spelling;
- the header limit applies before the terminator is found;
- the body limit is checked before buffering the body;
- a framing error poisons the parser for that connection; the caller closes it rather than attempting stream resynchronization;
- the framer does not parse JSON;
- returned bodies exclude headers and own their bytes.

Stable framing errors use `rpc.framing.*` codes, including `header_too_large`, `invalid_header`, `missing_content_length`, `duplicate_content_length`, `invalid_content_length`, `body_too_large`, and `unsupported_content_type`.

## 11. JSON-RPC protocol model

### 11.1 Public protocol values

Add `protocol.hpp`:

```cpp
namespace jb::rpc {

struct NullRequestId {
    auto operator==(NullRequestId const&) const -> bool = default;
};

using RequestId = std::variant<NullRequestId,
                               std::int64_t,
                               std::uint64_t,
                               std::string>;

enum class ErrorCode : std::int64_t {
    ParseError       = -32700,
    InvalidRequest   = -32600,
    MethodNotFound   = -32601,
    InvalidParams    = -32602,
    InternalError    = -32603,
    ApplicationError = -32000,
};

struct RpcError {
    std::int64_t             code{static_cast<std::int64_t>(ErrorCode::InternalError)};
    std::string              message;
    std::optional<JsonValue> data;

    auto operator==(RpcError const&) const -> bool = default;
};

struct PeerIdentity {
    std::optional<std::uint64_t> process_id;
    std::optional<std::uint64_t> user_id;
    std::optional<std::uint64_t> group_id;
};

struct OperationContext {
    PeerIdentity               peer;
    std::optional<std::string> authenticated_principal;
};

using ConnectionId = std::uint64_t;

struct RequestContext {
    ConnectionId    connection_id{0};
    OperationContext operation;
};

using MethodResult = jb::core::Result<JsonValue, RpcError>;
using MethodHandler = std::function<MethodResult(
    RequestContext const&, std::optional<JsonValue> const&)>;

[[nodiscard]] auto application_error(jb::core::Error const& error) -> RpcError;

} // namespace jb::rpc
```

All declarations and fields receive complete Doxygen comments in the actual header.

`application_error()` maps a `jb::core::Error` to numeric code `-32000`. Its `data` object contains the stable string `code` and a lower-case category name. The user-safe `message` is sent. Backend `detail` is logged server-side when appropriate but is not transmitted.

### 11.2 Envelope validation and encoding

Envelope structures and codec functions remain private to `rpc`. Applications use `Client`, `Server`, `JsonValue`, `RequestId`, and `RpcError`, not raw envelope constructors.

Required behavior:

- `jsonrpc` must exist and equal string `"2.0"`;
- request `method` must be a string;
- optional `params` must be an object or array;
- absent `id` means notification;
- an included ID must be null, an integral number, or a string; fractional IDs are invalid;
- responses require `id` and exactly one of `result` or `error`;
- error objects require an integral `code` and string `message`; `data` is optional;
- member names are case-sensitive;
- unknown request/response members are ignored for additive compatibility;
- method names beginning with `rpc.` remain reserved and cannot be registered as application handlers;
- duplicate JSON members have already been rejected by the JSON codec.

Support JSON-RPC batches within the same body limit:

- a non-empty top-level array is a batch;
- an empty array yields one `Invalid Request` response;
- each invalid batch element yields its own `Invalid Request` response with null ID;
- handlers run sequentially in input order on the event-loop thread;
- responses are emitted in the corresponding order;
- notifications never create responses;
- an all-notification batch produces no output;
- cap a batch at 64 entries by default; a larger batch yields `Invalid Request` without dispatching any entry.

The client initially sends one request per frame but accepts and correlates either a single response or a response batch. A public client batch-building API is unnecessary in Phase 2.

## 12. RPC server

Add `server.hpp`:

```cpp
namespace jb::rpc {

struct ServerOptions {
    FramingLimits framing;
    JsonLimits    json;
    std::size_t   max_batch_entries{64};
    std::size_t   max_connections{128};
    std::size_t   max_queued_output_bytes{2U * 1024U * 1024U};
};

class Server final : public jb::core::Object {
public:
    explicit Server(ServerOptions options = {}, jb::core::Object* parent = nullptr);
    ~Server() override;

    auto register_method(std::string name, MethodHandler handler) -> bool;
    auto unregister_method(std::string_view name) -> bool;
    [[nodiscard]] auto has_method(std::string_view name) const noexcept -> bool;

    [[nodiscard]] auto add_connection(
        std::unique_ptr<jb::core::IODevice> device,
        OperationContext operation = {})
        -> jb::core::Result<ConnectionId, jb::core::Error>;

    void close_connection(ConnectionId id);
    void close();

    [[nodiscard]] auto connection_count() const noexcept -> std::size_t;

    jb::core::Signal<ConnectionId>                         connection_opened;
    jb::core::Signal<ConnectionId>                         connection_closed;
    jb::core::Signal<ConnectionId, jb::core::Error>        connection_error;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::rpc
```

Registration behavior:

- method names and handlers must be non-empty;
- `rpc.` names are rejected;
- duplicate registration returns false and does not replace the original handler;
- registration/removal are ordinary event-loop-thread configuration operations and intentionally are not `[[nodiscard]]`;
- handler exceptions are caught, logged, and converted to `Internal Error` without terminating the daemon.

Connection behavior:

- `Server` takes exclusive ownership of an already-open device;
- the device must have an event loop and must not be read or written elsewhere afterward;
- each connection owns its framer and output accounting;
- valid framing plus invalid JSON produces `Parse Error` and keeps the connection open;
- invalid requests produce `Invalid Request` and keep the connection open;
- unknown methods produce `Method Not Found`;
- handler errors become JSON-RPC error responses;
- notification handler failures may be logged but never produce responses;
- a framing error, short device write, output-limit violation, or device error closes only that connection;
- `IODevice::closed` removes the connection and releases all state;
- writing a response never recursively processes another request;
- `close()` closes and destroys all connections but leaves method registrations intact;
- connection IDs are nonzero, monotonically allocated, and never reused while live.

The server's finite output accounting prevents a client that stops reading from growing daemon memory without bound. Bytes are removed from the count only through `IODevice::bytes_written`.

## 13. RPC client

Add `client.hpp`:

```cpp
namespace jb::rpc {

struct ClientOptions {
    FramingLimits framing;
    JsonLimits    json;
    std::size_t   max_batch_entries{64};
    std::size_t   max_pending_requests{128};
    std::size_t   max_queued_output_bytes{2U * 1024U * 1024U};
};

class Client final : public jb::core::Object {
public:
    explicit Client(jb::core::IODevice& device,
                    ClientOptions options = {},
                    jb::core::Object* parent = nullptr);
    ~Client() override;

    [[nodiscard]] auto call(
        std::string_view method,
        std::optional<JsonValue> params = std::nullopt)
        -> jb::core::Result<RequestId, jb::core::Error>;

    [[nodiscard]] auto notify(
        std::string_view method,
        std::optional<JsonValue> params = std::nullopt)
        -> jb::core::Result<void, jb::core::Error>;

    void cancel(RequestId const& id);
    void close();

    [[nodiscard]] auto pending_request_count() const noexcept -> std::size_t;

    jb::core::Signal<RequestId, JsonValue>             result_received;
    jb::core::Signal<RequestId, RpcError>               error_received;
    jb::core::Signal<RequestId, jb::core::Error>        request_failed;
    jb::core::Signal<jb::core::Error>                   protocol_error;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::rpc
```

Client rules:

- the client borrows an open `IODevice`; the device must outlive the client and is exclusively read/written by it;
- calls allocate positive unsigned integer IDs starting at one;
- wrap-around skips IDs still pending;
- no request is recorded as pending until the complete framed write is accepted;
- multiple calls may remain outstanding and responses may arrive in any order;
- `cancel()` only forgets local correlation and emits no wire message;
- an error response emits `error_received` and removes the request;
- a successful response emits `result_received` and removes the request;
- a valid response with null, unknown, or duplicate ID is a protocol violation;
- malformed JSON, invalid response envelopes, response batches above the limit, short writes, or output overflow emit `protocol_error` and close the client;
- device close/error emits `request_failed` once for each pending request before clearing them;
- `close()` disconnects signal subscriptions and fails pending requests without assuming ownership of the borrowed device;
- no timeout is hidden inside `Client`; `jobuctl` owns its command timeout through `jb::core::Timer`.

Method/parameter validation occurs before writing. Params, when present, must be an object or array. Calls that fail this programmer-facing validation return a stable `rpc.invalid_argument` error; the existing `Result` attribute already requires callers to consider the outcome.

## 14. `system.info` application contract

Add `system_info.hpp` under `jb::jobu`:

```cpp
namespace jb::jobu {

struct ApiVersion {
    std::uint32_t major{1};
    std::uint32_t minor{0};
};

struct SystemInfo {
    std::string              daemon_version;
    ApiVersion               api_version;
    std::vector<std::string> capabilities;
};

[[nodiscard]] auto system_info_to_json(SystemInfo const& info)
    -> jb::rpc::JsonValue;

[[nodiscard]] auto system_info_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<SystemInfo, jb::core::Error>;

} // namespace jb::jobu
```

The actual header fully documents every type, field, and function. `system_info_from_json()` requires known mandatory fields with correct types and ignores unknown fields.

The wire result is:

```json
{
  "daemon_version": "0.1.0",
  "api_version": {
    "major": 1,
    "minor": 0
  },
  "capabilities": [
    "system.info"
  ]
}
```

`system.info` accepts absent params or an empty object. Any supplied field or positional value returns `Invalid Params`. Capabilities are unique and sorted for deterministic output.

### 14.1 `jobud`

Phase 2 adds only a foreground diagnostic service:

```text
jobud --socket <filesystem-path>
```

It:

1. constructs `Application`, `LocalServer`, and `jb::rpc::Server`;
2. registers `system.info`;
3. listens with owner read/write permissions;
4. drains every `new_connection` signal;
5. copies `LocalPeerCredentials` into `OperationContext`;
6. transfers each accepted socket to the RPC server;
7. enters the event loop.

Database startup, config files, privilege changes, stale-path recovery, and graceful signal shutdown remain deferred. Existing `--version` behavior stays intact. Missing/invalid arguments return a usage error rather than silently choosing a shared `/tmp` path.

### 14.2 `jobuctl`

Phase 2 accepts:

```text
jobuctl --socket <filesystem-path> system info
```

It:

1. creates `Application`, `LocalSocket`, and `jb::rpc::Client`;
2. connects asynchronously;
3. sends `system.info` after `connected`;
4. decodes the typed result;
5. prints daemon/API version and capabilities;
6. exits zero after a valid response;
7. exits nonzero for usage, connect, timeout, framing/protocol, remote, or DTO validation errors.

Use a visible one-shot command timer, default five seconds, owned by `jobuctl`. Do not add blocking socket waits or nested event loops.

## 15. Stable errors

### 15.1 Local transport

Local IPC continues to use `jb::core::IOError` plus a user-safe message, matching existing `IODevice` APIs. Required classifications include invalid path/options, not open, open/connect/bind/listen failure, read/write failure, permission denial, and resource exhaustion.

### 15.2 RPC implementation errors

Use stable `jb::core::Error::code` strings for local implementation failures:

| Code | Meaning |
| --- | --- |
| `rpc.invalid_argument` | invalid local API input |
| `rpc.connection_limit` | server connection limit reached |
| `rpc.connection_closed` | device closed with a request outstanding |
| `rpc.pending_limit` | client pending-request limit reached |
| `rpc.output_limit` | queued output would exceed the configured limit |
| `rpc.short_write` | device did not accept a complete frame |
| `rpc.protocol_error` | peer sent an invalid response/envelope |
| `rpc.json.*` | JSON codec failure |
| `rpc.framing.*` | stream framing failure |

These are not sent as JSON-RPC errors unless they arise from dispatchable server-side application work.

### 15.3 JSON-RPC errors

Use the standard numeric codes exactly:

| Numeric code | Meaning |
| ---: | --- |
| `-32700` | Parse error |
| `-32600` | Invalid Request |
| `-32601` | Method not found |
| `-32602` | Invalid params |
| `-32603` | Internal error |
| `-32000` | JobU application error; stable string code in `data.code` |

Do not send native errno values, socket paths, JSON bodies, SQL, secret data, or backend diagnostic detail in protocol errors.

## 16. Testing design

### 16.1 Public boundaries and documentation review

Add direct-include tests:

```cpp
// net-public-headers-test.cpp
#include "local_server.hpp"
#include "local_socket.hpp"
```

```cpp
// rpc-public-headers-test.cpp
#include "client.hpp"
#include "framing.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "rpc.hpp"
#include "server.hpp"
```

Extend the aggregate public-header test with `system_info.hpp`.

For each stage, Codex must inspect every public header in that stage and report Doxygen coverage. Compilation tests prove include independence but do not replace this documentation review.

### 16.2 Deterministic memory device

The test-only `MemoryIODevice` supports:

- explicit open/close;
- injecting fragmented input and emitting `ready_read`;
- inspecting/taking bytes written by the RPC layer;
- limiting or forcing short writes;
- injecting I/O errors;
- observing close and error ordering.

It lets JSON, framing, server, client, batch, correlation, and connection-failure tests run without real sockets or sleeps.

### 16.3 JSON and framing tests

Cover:

- every JSON alternative and nested round trips;
- deterministic object output;
- UTF-8, escaping, integer limits, non-finite numbers, duplicate members, and depth limits;
- header/body fragmentation at every byte boundary;
- several coalesced messages;
- incomplete input retained across appends;
- CRLF enforcement;
- header-name casing and ignored future headers;
- missing, duplicate, malformed, overflowing, zero, and excessive content lengths;
- content type handling;
- exact header and body boundary sizes;
- reset after successful use and refusal to resynchronize after framing error.

### 16.4 Protocol/server tests

Cover:

- request, notification, response, and error envelopes;
- signed, unsigned, string, and null IDs;
- absent/object/array params and primitive-param rejection;
- all standard error responses;
- unknown members ignored;
- reserved `rpc.` and duplicate method registration;
- method success, application error, exception, and notification failure;
- empty, mixed, invalid-element, all-notification, and over-limit batches;
- connection limit, output limit, short write, malformed framing, malformed JSON, and device close;
- one broken connection not affecting another.

### 16.5 Client tests

Cover:

- monotonically generated IDs;
- multiple outstanding calls and out-of-order responses;
- remote errors;
- notifications and no pending entry;
- response batches;
- local cancellation followed by a now-unknown response;
- unknown, null, duplicate, fractional, or malformed response IDs;
- pending/output limits;
- device error/close failing every pending request exactly once;
- borrowed-device lifetime and signal disconnection on client destruction.

### 16.6 Linux local IPC tests

Use unique paths inside `TemporaryDirectory`. Cover:

- asynchronous connect and accept;
- waiting for both client connection and server pending acceptance before asserting, avoiding the race already encountered in TCP tests;
- bidirectional binary data, fragmented data, queued writes, graceful close, abort, and final buffered reads;
- input-buffer backpressure and resume;
- multiple pending clients and pending-limit behavior;
- nonexistent parent, existing regular file, duplicate listener, and overlong path;
- socket mode after listen;
- listener close path cleanup and inode-mismatch protection;
- Linux client PID/UID/GID through `SO_PEERCRED`;
- no descriptor leakage across repeated open/close cycles.

### 16.7 Linux RPC and executable tests

First run an in-process client/server over `LocalSocket` and `LocalServer`, then add an executable integration test that:

1. starts foreground `jobud` with a unique socket path;
2. waits with a bounded deadline for the server to listen;
3. runs `jobuctl --socket <path> system info`;
4. verifies exit status and output fields;
5. terminates the test daemon using test-process facilities;
6. cleans the temporary directory.

Graceful daemon shutdown is not asserted in Phase 2. The test must have a hard deadline and must never leave a child process running after failure.

### 16.8 macOS tests

After all Linux stages are complete, run the same behavioral suite on macOS with platform-specific credential expectations:

- UID/GID are present;
- PID may be absent;
- `SO_NOSIGPIPE` prevents process termination on peer close;
- nonblocking and close-on-exec flags are verified where observable;
- local RPC and `jobuctl system info` pass through the kqueue event loop.

If Codex does not have access to a macOS environment, it must stop at the macOS stage and report that validation is unavailable. It must not claim the stage complete based only on Linux compilation or code inspection.

## 17. Reviewable implementation sequence

Each stage is a separate approval, implementation, validation, and review boundary. Codex implements exactly one stage, reports changed files and validation, and waits for approval before continuing.

### Stage 2.1: Generic `IODevice` close notification

- add and document `IODevice::closed` and protected emission support;
- update `File` and `TcpSocket` without changing their existing public behavior;
- add exact-once and signal-order tests.

Exit: an arbitrary RPC transport can observe end-of-stream through `IODevice` alone.

### Stage 2.2: Project-owned JSON value and codec

- add fully documented `json.hpp`;
- add private nlohmann/json conversion;
- add JSON correctness, limit, and dependency-boundary tests;
- update the RPC public-header target.

Exit: JSON can be parsed and serialized without exposing nlohmann/json.

### Stage 2.3: Bounded stream framing

- add fully documented `framing.hpp`;
- implement incremental LSP-style parsing and encoding;
- add exhaustive fragmentation, coalescing, malformed-input, and boundary tests.

Exit: arbitrary byte fragments produce complete bounded bodies or one stable framing error.

### Stage 2.4: JSON-RPC envelopes and validation

- add fully documented public protocol value/error/context types;
- keep envelope codecs private;
- implement single and batch validation/encoding;
- add protocol-only tests for all standard errors and compatibility rules.

Exit: valid JSON trees can be transformed to/from strict JSON-RPC messages without I/O.

### Stage 2.5: Transport-independent RPC server

- add fully documented `server.hpp`;
- add the deterministic memory `IODevice`;
- implement handler registration, dispatch, connection ownership, limits, and isolation;
- test entirely with memory devices.

Exit: the server dispatches framed requests and notifications with no dependency on `net`.

### Stage 2.6: Transport-independent RPC client

- add fully documented `client.hpp`;
- implement calls, notifications, IDs, correlation, cancellation, limits, and connection failure;
- test multiple outstanding and out-of-order responses using memory devices.

Exit: client and server complete full in-memory round trips and remain independent of local sockets.

### Stage 2.7: Linux `LocalSocket`

- add fully documented `local_socket.hpp`;
- implement only `local_socket_linux.cpp` and its private state;
- add Linux client-side socket, buffering, close, credential, and error tests;
- add the net public-header test.

Exit: a Linux `LocalSocket` exchanges bytes asynchronously with a small native test listener.

### Stage 2.8: Linux `LocalServer`

- add fully documented `local_server.hpp`;
- implement only `local_server_linux.cpp` and its private state;
- test accept queues, permissions, cleanup safety, limits, credentials, and multiple clients.

Exit: Linux clients and servers communicate entirely through project-owned local IPC APIs.

### Stage 2.9: Linux local RPC integration

- compose `LocalServer`, accepted `LocalSocket`, `rpc::Server`, `LocalSocket`, and `rpc::Client` in tests;
- map peer credentials into `OperationContext`;
- test persistent connections, several clients, fragmentation, limits, and connection isolation.

Exit: the complete reusable RPC stack works over Linux Unix-domain sockets.

### Stage 2.10: Linux `system.info`, `jobud`, and `jobuctl`

- add and fully document `system_info.hpp`;
- implement the `system.info` handler and typed conversion;
- implement minimal `--socket` command handling in both executables;
- add Linux executable integration with hard cleanup deadlines;
- preserve `--version` behavior.

Exit: `jobuctl --socket <path> system info` succeeds against foreground `jobud` on Linux.

No macOS source or macOS-specific conditional is implemented before this exit is demonstrated.

### Stage 2.11: macOS `LocalSocket`

- implement `local_socket_macos.cpp` against the already frozen public contract;
- use `fcntl`, `SO_NOSIGPIPE`, and `getpeereid` as applicable;
- run the socket client behavior suite on macOS.

Exit: macOS `LocalSocket` matches Linux-visible behavior with documented credential differences.

### Stage 2.12: macOS `LocalServer`

- implement `local_server_macos.cpp` against the frozen public contract;
- run accept, permissions, cleanup, pending-limit, and credential tests on macOS;
- make final CMake source selection explicit for Linux and Darwin.

Exit: macOS local client/server byte exchange passes without changing the public API.

### Stage 2.13: macOS RPC and executable verification

- run the in-process local RPC suite on macOS;
- run foreground `jobud` plus `jobuctl system info` on macOS;
- correct only platform implementation defects discovered by these tests;
- run the complete Linux suite again to guard against regression.

Exit: the Phase 2 exit criteria pass on Linux and macOS.

## 18. Validation commands

Run after every stage on the stage's active platform:

```sh
cmake -B .bld -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

Running the full test suite requires permissions to run it outside the sandbox.

Also:

- format every changed C++ file with the checked-in `.clang-format`;
- run focused test executables before the complete suite;
- inspect new public headers for direct include completeness and Doxygen coverage;
- search public headers for `nlohmann`, native local-socket types, `sys/socket.h`, `sys/un.h`, Linux `ucred`, and macOS `xucred`;
- inspect target link interfaces to confirm nlohmann/json is private to `rpc`;
- use ASAN/UBSAN for memory-device, framing, protocol, and connection-lifetime tests where available;
- use unique temporary socket paths and bounded deadlines;
- do not use unbounded waits or timing-sensitive sleeps in unit tests;
- verify no test leaves socket files, descriptors, or child processes behind.

Linux stages require GCC and Clang coverage before starting macOS work. macOS stages require Apple Clang and the kqueue backend. Final Phase 2 acceptance requires both platforms.

## 19. Review invariants

Before Phase 2 is considered complete, review these invariants directly:

- `rpc` has no dependency on `net`;
- `LocalSocket` and `LocalServer` contain no JSON or RPC logic;
- public headers expose no dependency-specific or OS-native types;
- Linux and macOS implementations are in separate source files;
- no macOS implementation work appears before Stage 2.11;
- every RPC input has a finite header, body, nesting, batch, pending-request, connection, and queued-output limit;
- a framing failure closes one connection and never attempts resynchronization;
- a framed JSON parse failure returns `-32700` without closing a healthy server connection;
- notifications never receive responses;
- response IDs are correlated exactly and cannot complete a request twice;
- all pending client requests fail exactly once when their device closes;
- `system.info` ignores unknown result fields and advertises only implemented capabilities;
- no database, migration, queue, scheduler, runner, authentication, or privilege code has entered Phase 2;
- every new public header and public declaration is documented with Doxygen comments.

## 20. Reference points

The wire behavior follows:

- [JSON-RPC 2.0 Specification](https://www.jsonrpc.org/specification);
- [Language Server Protocol base framing](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#baseProtocol).

The API shape uses Qt as a familiarity reference where practical:

- `QLocalSocket` for asynchronous connect/read/write/state concepts;
- `QLocalServer` for listen/new-connection/pending-connection concepts;
- `QJsonValue` for a project-owned JSON tree;
- Qt signal-driven networking generally.

JobU intentionally differs through explicit ownership, no implicit shared handles, project-owned error/result types, finite protocol limits, and no Qt dependency.
