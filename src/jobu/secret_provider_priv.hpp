#pragma once

#include "secret_provider.hpp"

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

/// Borrows an open owner-thread database, which must outlive this provider.
/// Construction performs no I/O. Lookups borrow any active transaction and never begin, commit, or roll it back.
class DatabaseSecretProvider final : public SecretProvider {
public:
    explicit DatabaseSecretProvider(jb::db::Database& database) noexcept;
    [[nodiscard]] auto resolve(std::string_view name)
        -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error> override;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
