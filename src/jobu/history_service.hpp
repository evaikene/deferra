/// @file history_service.hpp
/// @brief Owner-thread lifetime boundary for retained history reads.
///
#pragma once

#include "error.hpp"
#include "object.hpp"
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

    /// Irreversibly closes read admission and discards every cursor. Idempotent and owner-thread only.
    void shutdown() noexcept;

    /// Emitted at most once for a fatal read or persisted-data failure, after query resources are released.
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
};

} // namespace jb::jobu
