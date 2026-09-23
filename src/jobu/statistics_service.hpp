/// @file statistics_service.hpp
/// @brief Owner-thread retained-history statistics reads.
///
#pragma once

#include "error.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"
#include "statistics.hpp"

namespace jb::db {
class Database;
}

namespace jb::core {
class TimeSource;
class UuidGenerator;
} // namespace jb::core

namespace jb::jobu {

/// Reads bounded statistics over immutable planned-run cohorts and their retained attempts.
/// Borrows an already-open owner-thread database, UUID generator, and time source; they must outlive the service.
/// Construction does no database work. Reads and shutdown run on that owner thread. A fatal storage or persisted-data
/// failure closes admission and emits one sanitized failed signal after query handles have been released.
/// Ordinary database read errors fail only that request. The optional parent owns the service.
class StatisticsService final : public jb::core::Object {
public:
    StatisticsService(jb::db::Database&        database,
                      jb::core::UuidGenerator& uuid_generator,
                      jb::core::TimeSource&    time_source,
                      jb::core::Object*        parent = nullptr);
    ~StatisticsService() override;

    StatisticsService(StatisticsService const&)                    = delete;
    StatisticsService(StatisticsService&&)                         = delete;
    auto operator=(StatisticsService const&) -> StatisticsService& = delete;
    auto operator=(StatisticsService&&) -> StatisticsService&      = delete;

    /// Returns one aggregate for None, or a keyset page of groups in canonical UUID-byte or enum-wire order.
    /// Queue scope requires a resolved queue ID on its initial request; continuations contain only a cursor and
    /// must use the same scope. Windows are fixed at initial read; counts remain a live view of retained history.
    /// Invalid input returns `jobu.statistics.invalid_request` (InvalidArgument); invalid, expired, or wrong-scope
    /// cursors return `jobu.statistics.invalid_cursor` (InvalidArgument). Token generation failure returns
    /// `jobu.statistics.cursor_unavailable` (ResourceExhausted). These fail only this read. Shutdown returns
    /// `jobu.service.stopping` (Unavailable); fatal storage errors close admission and emit `failed` once.
    [[nodiscard]] auto read(StatisticsListRequest const& request, StatisticsScope scope)
        -> jb::core::Result<StatisticsPage, jb::core::Error>;

    /// Irreversibly closes read admission and clears cursor state. Idempotent and owner-thread only.
    void shutdown() noexcept;

    /// Emitted at most once for a fatal read, after all query handles have been released.
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
};

} // namespace jb::jobu
