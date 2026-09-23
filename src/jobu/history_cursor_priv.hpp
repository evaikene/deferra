#pragma once

#include "history.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace jb::jobu::detail {

/// Immutable keyset boundary for a descending run-history page.
struct RunCursorKey {
    jb::core::UtcTimePoint planned_at;
    jb::core::Uuid         id;
};

/// The full initial query and boundary owned by one run cursor.
struct RunCursorState {
    RunQuery            query;
    RunCursorKey        after;
    jb::core::TimePoint expires_at;
};

/// The run and descending attempt-number boundary owned by one attempt cursor.
struct AttemptCursorState {
    AttemptQuery        query;
    AttemptNumber       before_attempt_number{0};
    jb::core::TimePoint expires_at;
};

/// Bounded owner-thread cursor storage. Tokens contain no filters or SQL.
/// Lookup changes only LRU recency; a successor gets a new token with the original expiry.
class HistoryCursorStore final {
public:
    /// Maximum simultaneous entries; expired entries are removed before LRU eviction.
    static constexpr std::size_t maximum_entries = 1024;

    /// Borrows injectable identity and monotonic-clock collaborators on one owner thread.
    HistoryCursorStore(jb::core::UuidGenerator& uuid_generator, jb::core::TimeSource& time_source);

    /// Stores an initial run boundary with an expiry five minutes from now.
    /// Generation failure reports jobu.history.cursor_unavailable without exposing generator detail.
    [[nodiscard]] auto start_run(RunQuery query, RunCursorKey after) -> jb::core::Result<std::string, jb::core::Error>;
    /// Stores an initial attempt boundary under the same lifetime and size policy.
    [[nodiscard]] auto start_attempt(AttemptQuery query, AttemptNumber before_attempt_number)
        -> jb::core::Result<std::string, jb::core::Error>;

    /// Returns a copy of the stored state. Every invalid token has one fixed jobu.history.invalid_cursor error.
    [[nodiscard]] auto get_run(std::string_view token) -> jb::core::Result<RunCursorState, jb::core::Error>;
    /// Returns a copy only when the token belongs to attempt.list.
    [[nodiscard]] auto get_attempt(std::string_view token) -> jb::core::Result<AttemptCursorState, jb::core::Error>;

    /// Issues a distinct successor with the original expiry; the input token remains retryable.
    [[nodiscard]] auto advance_run(RunCursorState state, RunCursorKey after)
        -> jb::core::Result<std::string, jb::core::Error>;
    /// Issues a distinct attempt successor with the original expiry.
    [[nodiscard]] auto advance_attempt(AttemptCursorState state, AttemptNumber before_attempt_number)
        -> jb::core::Result<std::string, jb::core::Error>;

    /// Discards all tokens, including on service shutdown.
    void               clear() noexcept;
    /// Reports entries retained since the last expiry cleanup.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

private:
    using State = std::variant<RunCursorState, AttemptCursorState>;

    enum class Family : std::uint8_t {
        Run,
        Attempt
    };

    struct Entry {
        std::string token;
        State       state;
    };

    [[nodiscard]] auto issue(State state) -> jb::core::Result<std::string, jb::core::Error>;
    [[nodiscard]] auto find(std::string_view token, Family family) -> jb::core::Result<State, jb::core::Error>;
    void               evict_expired(jb::core::TimePoint now);

    jb::core::UuidGenerator&                                    _uuid_generator;
    jb::core::TimeSource&                                       _time_source;
    std::list<Entry>                                            _entries;
    std::unordered_map<std::string, std::list<Entry>::iterator> _by_token;
};

} // namespace jb::jobu::detail
