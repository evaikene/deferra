#include "secret_provider_priv.hpp"

#include "secret_repository_priv.hpp"

namespace jb::jobu::detail {

DatabaseSecretProvider::DatabaseSecretProvider(jb::db::Database& database) noexcept
    : _database{database}
{}

auto DatabaseSecretProvider::resolve(std::string_view name) -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error>
{
    return SecretRepository{_database}.find_value(name);
}

} // namespace jb::jobu::detail
