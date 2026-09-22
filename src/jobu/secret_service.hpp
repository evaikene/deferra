/// @file secret_service.hpp
/// @brief Checked owner-thread secret writes and metadata reads, independent of transport.
#pragma once

#include "object.hpp"
#include "result.hpp"
#include "secret.hpp"
#include "signal.hpp"

#include <string_view>

namespace jb::db {
class Database;
}

namespace jb::jobu {

/// Borrows an already-open sole-writer database and clock, both of which must outlive this service.
/// All calls and signal delivery belong to the database owner thread. Operations are synchronous and cannot reenter
/// this service or run within a caller-owned transaction. Construction performs no database work or notification.
/// State lives in the single Object private block; an optional parent owns the service as usual.
///
/// Fatal storage/invariant failures irreversibly stop mutations. Reads remain available after stop_mutations(),
/// using Read failure policy: ordinary read I/O errors are nonfatal, corruption/invariants and poisoning are fatal.
/// Values remain plaintext BLOBs in storage; this API does not provide encryption or secure allocator erasure.
class SecretService final : public jb::core::Object {
public:
    /// Borrows database and time_source on their owner thread; parent optionally supplies ownership and affinity.
    SecretService(jb::db::Database& database, jb::core::TimeSource& time_source, jb::core::Object* parent = nullptr);
    /// Releases private state without closing or changing the borrowed database.
    ~SecretService() override;

    SecretService(SecretService const&)                    = delete;
    SecretService(SecretService&&)                         = delete;
    auto operator=(SecretService const&) -> SecretService& = delete;
    auto operator=(SecretService&&) -> SecretService&      = delete;

    /// Upserts 0–65,536 raw bytes under a canonical name, in one immediate transaction.
    /// Preserves creation time, samples the clock inside the transaction and replaces value/update time.
    /// Returns metadata only; invalid_name, too_large, stopped admission and storage errors contain safe fixed text.
    [[nodiscard]] auto set(SetSecretRequest request) -> jb::core::Result<SecretMetadata, jb::core::Error>;

    /// Reads at most limit metadata items in ascending name order, exclusively after after_name when supplied.
    /// Limit must be 1–200 and the boundary must be a canonical name. Queries never select secret values.
    /// Returns a continuation only when another item exists; this is a live view across calls.
    [[nodiscard]] auto list(SecretListRequest const& request) -> jb::core::Result<SecretPage, jb::core::Error>;

    /// Deletes one named secret transactionally; the name is borrowed only for the call.
    /// Missing names return jobu.secret.not_found. Current definitions and all nonterminal run snapshots
    /// protect references with jobu.secret.in_use; terminal history alone does not prevent deletion.
    /// Checks and deletion share one transaction. Snapshot scanning is bounded in memory, not total latency.
    /// Malformed stored templates fail closed through the service failure boundary.
    [[nodiscard]] auto erase(std::string_view name) -> jb::core::Result<void, jb::core::Error>;

    /// Irreversibly rejects subsequent writes with Unavailable / jobu.service.stopping. Reads remain available.
    /// Owner-thread only, idempotent, and safe from a failure slot; performs no database work or notification.
    void stop_mutations() noexcept;

    /// Emitted synchronously once for each successful set/erase, after commit and all query/transaction cleanup.
    /// Reads, rejected operations and failed operations never emit. Slots may request coalesced later work but must
    /// not use the database, reenter the service, destroy it, block or process nested events during delivery.
    jb::core::Signal<> mutation_committed;

    /// Emitted synchronously at most once for the first fatal failure, after query/transaction cleanup and gate
    /// closure. Failed rollback poisoning outranks an ordinary error; an already fatal operation error is preserved.
    /// The safe error reference is borrowed for delivery. Slots may latch shutdown but must not destroy/reenter the
    /// service or process nested events. Connect with a receiver whenever a slot captures an Object.
    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
    auto set_impl(SetSecretRequest const& request) -> jb::core::Result<SecretMetadata, jb::core::Error>;
    auto list_impl(SecretListRequest const& request) -> jb::core::Result<SecretPage, jb::core::Error>;
    auto erase_impl(std::string_view name) -> jb::core::Result<void, jb::core::Error>;
};

} // namespace jb::jobu
