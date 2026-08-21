/** @file framing.hpp
 * @brief Defines bounded LSP-style byte-stream framing and encoding.
 */
#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jb::rpc {

/** Configures the maximum memory accepted for one framed message.
 *
 * Header limits count bytes through and including the terminating `\r\n\r\n`; body limits count only body bytes.
 * Both boundaries are inclusive. Bodies are opaque bytes and are not required to contain UTF-8 or JSON.
 */
struct FramingLimits {
    /// Maximum permitted header size in bytes, including the terminating `\r\n\r\n`.
    std::size_t max_header_bytes{std::size_t{16} * 1024U};
    /// Maximum permitted body size in bytes; zero-length bodies remain valid when this limit is zero.
    std::size_t max_body_bytes{std::size_t{1024} * 1024U};
};

/** Incrementally extracts owned bodies from an LSP-style byte stream.
 *
 * Input may be fragmented at any byte or contain several frames. Incomplete input is retained within the configured
 * limits. A framing failure clears retained bytes and poisons the framer: later append operations return the same error
 * until reset() is called for deliberate reuse after abandoning the failed connection. Returned bodies own their bytes
 * and exclude framing headers; no body is interpreted as UTF-8 or JSON.
 */
class StreamFramer final {
public:
    /** Creates an empty framer with fixed resource limits.
     * @param limits Inclusive header and body byte limits used for the lifetime of this parser state.
     */
    explicit StreamFramer(FramingLimits limits = {});

    /** Destroys the framer and any incomplete or poisoned state it owns. */
    ~StreamFramer();

    /// Framer state is uniquely owned and cannot be copied.
    StreamFramer(StreamFramer const&) = delete;

    /** Transfers buffered or poisoned state from another framer.
     * @param other Framer whose state is transferred; afterward it may only be destroyed or move-assigned.
     */
    StreamFramer(StreamFramer&& other) noexcept;

    /// Framer state is uniquely owned and cannot be copy-assigned.
    auto operator=(StreamFramer const&) -> StreamFramer& = delete;

    /** Replaces this framer's state by transferring another framer's state.
     * @param other Framer whose state is transferred; afterward it may only be destroyed or move-assigned.
     * @return This framer.
     */
    auto operator=(StreamFramer&& other) noexcept -> StreamFramer&;

    /** Consumes the next byte-stream fragment.
     *
     * Complete bodies are returned in source order. An incomplete fragment succeeds with an empty vector and remains
     * buffered. If any framing in this fragment fails, the whole operation fails and no earlier bodies from the same
     * call are returned. Errors use stable `rpc.framing.*` codes and never include input bytes.
     *
     * @param bytes Borrowed fragment consumed only for this call; arbitrary byte values are accepted in bodies.
     * @return Complete owned bodies, or the first framing error for this parser state.
     */
    [[nodiscard]] auto append(std::string_view bytes) -> jb::core::Result<std::vector<std::string>, jb::core::Error>;

    /** Clears incomplete input and poison while preserving configured limits.
     * @warning Reset is for reuse after abandoning the previous stream, not for resynchronizing a failed connection.
     */
    void reset() noexcept;

    /** Reports bytes currently retained for an incomplete header or body.
     * @return Owned incomplete-input bytes; zero after completion, failure, or reset.
     */
    [[nodiscard]] auto buffered_bytes() const noexcept -> std::size_t;

    /** Returns the immutable resource limits configured at construction.
     * @return Borrowed limits reference valid until this framer is destroyed or move-assigned.
     */
    [[nodiscard]] auto limits() const noexcept -> FramingLimits const&;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

/** Encodes one opaque body using deterministic LSP-style framing.
 *
 * The result contains only `Content-Length: <bytes>\r\n\r\n` followed by the body. The byte count is
 * `body.size()`; body contents are neither interpreted nor validated. Header and body limits are inclusive.
 *
 * @param body Borrowed bytes copied into the returned frame.
 * @param limits Inclusive byte limits applied before constructing the frame.
 * @return An owned frame, or a stable `rpc.framing.*` limit error.
 */
[[nodiscard]] auto frame_message(std::string_view body, FramingLimits limits = {})
    -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::rpc
