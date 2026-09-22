#pragma once

#include "byte_buffer.hpp"
#include "payload_template_priv.hpp" // IWYU pragma: export - shared reference row type
#include "result.hpp"
#include "secret.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

inline constexpr std::size_t kMaximumSecretValueBytes = std::size_t{64} * 1024U;

/// One immutable run's recognized references. No payload, result, output or secret bytes are retained.
struct RunSecretReferences {
    jb::core::Uuid               run_id;
    std::vector<SecretReference> references;
};

class SecretRepository final {
public:
    explicit SecretRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto set(std::string_view name, jb::core::ByteView value, jb::core::UtcTimePoint updated_at)
        -> jb::core::Result<SecretMetadata, jb::core::Error>;
    /// Trusted execution-only lookup. Returns owning raw bytes, including an empty BLOB; a missing row is not_found.
    /// Borrows the owner's connection/transaction, never starts a transaction or emits callbacks.
    /// Malformed or oversized stored values are durable invariant failures, not caller validation errors.
    [[nodiscard]] auto find_value(std::string_view name) -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error>;
    [[nodiscard]] auto list_metadata(std::size_t limit, std::optional<std::string_view> after_name = std::nullopt)
        -> jb::core::Result<std::vector<SecretMetadata>, jb::core::Error>;
    /// Checks distinct names using metadata only; missing names return jobu.secret.not_found.
    /// The caller holds the same transaction through definition/reference writes. Never starts a transaction.
    [[nodiscard]] auto require_existing_references(std::span<SecretReference const> references)
        -> jb::core::Result<void, jb::core::Error>;
    /// Scans all nonterminal snapshots regardless of owner state or run origin, in stored UUID order.
    /// Pages own their references and retain no query; limit is 1..200 and after_id is exclusive.
    /// Malformed durable templates fail the whole page. Hold one transaction across all pages and deletion.
    [[nodiscard]] auto list_nonterminal_references(std::size_t limit, std::optional<jb::core::Uuid> after_id = {})
        -> jb::core::Result<std::vector<RunSecretReferences>, jb::core::Error>;
    [[nodiscard]] auto erase(std::string_view name) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto replace_references_for_job(jb::core::Uuid const&            job_id,
                                                  std::span<SecretReference const> references)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto erase_references_for_queue(jb::core::Uuid const& queue_id)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto reference_count(std::string_view name) -> jb::core::Result<std::uint64_t, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
