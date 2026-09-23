/// @file history_service.hpp
/// @brief Owner-thread lifetime boundary for retained history reads.
///
#pragma once

#include "attempt_executor.hpp"
#include "error.hpp"
#include "history.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"

namespace jb::db {
class Database;
}

namespace jb::core {
class TimeSource;
class UuidGenerator;
} // namespace jb::core

namespace jb::jobu {

class AttributeRegistry;

/// Owns bounded history cursor state on the database owner thread.
/// Borrows the open database, immutable attribute registry, UUID generator, and time source; all must outlive it.
/// Construction performs no database work or notification. Read operations will use the same thread and will signal
/// one sanitized fatal storage or persisted-data failure after query cleanup. An optional parent owns the service.
///
/// `jobu.history.invalid_request` and `jobu.history.invalid_cursor` (InvalidArgument), plus `jobu.run.not_found` and
/// `jobu.attempt.not_found` (NotFound), fail only the requested read. `jobu.response.too_large` (ResourceExhausted)
/// means one item cannot fit a bounded page; it also fails only that read. Malformed durable data and corrupt storage
/// close admission and emit `failed` once. Other database read errors fail the operation without closing admission.
class HistoryService final : public jb::core::Object {
public:
    /// Borrows collaborators and creates empty cursor state without querying the database.
    HistoryService(jb::db::Database&        database,
                   AttributeRegistry const& attributes,
                   jb::core::UuidGenerator& uuid_generator,
                   jb::core::TimeSource&    time_source,
                   jb::core::Object*        parent = nullptr);
    /// Releases cursor state without closing the borrowed database.
    ~HistoryService() override;

    HistoryService(HistoryService const&)                    = delete;
    HistoryService(HistoryService&&)                         = delete;
    auto operator=(HistoryService const&) -> HistoryService& = delete;
    auto operator=(HistoryService&&) -> HistoryService&      = delete;

    /// Returns the retained run snapshot, including its original template and safe result.
    /// A missing run returns jobu.run.not_found; storage failures follow the owner-thread fatal-read policy.
    [[nodiscard]] auto get_run(jb::core::Uuid const& id) -> jb::core::Result<RunDetails, jb::core::Error>;
    /// Lists lightweight run summaries in descending planned-time/UUID order.
    /// An initial request accepts limits 1–200; a continuation uses only its opaque cursor.
    [[nodiscard]] auto list_runs(RunListRequest const& request) -> jb::core::Result<RunPage, jb::core::Error>;
    /// Returns one attempt's retained result without fetching captured output.
    [[nodiscard]] auto get_attempt(AttemptKey const& key) -> jb::core::Result<AttemptDetails, jb::core::Error>;
    /// Lists lightweight attempts for one run, newest attempt number first.
    [[nodiscard]] auto list_attempts(AttemptListRequest const& request)
        -> jb::core::Result<AttemptPage, jb::core::Error>;

    /// Irreversibly closes read admission and discards every cursor. Idempotent and owner-thread only.
    void shutdown() noexcept;

    /// Emitted at most once for a fatal read or persisted-data failure, after query resources are released.
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
};

} // namespace jb::jobu
