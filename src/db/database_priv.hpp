#pragma once

#include "database.hpp"

#include <cstddef>
#include <thread>
#include <utility>

namespace jb::db {

enum class TransactionOwner : std::uint8_t {
    None,
    Direct,
    Guarded,
};

inline auto make_database_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

struct Database::Private {
    explicit Private(std::unique_ptr<Driver> value = {})
        : driver{std::move(value)}
    {}

    [[nodiscard]] auto operation_error() const -> std::optional<jb::core::Error>
    {
        if (!driver) {
            return make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                       "db.invalid_database",
                                       "The database has no driver");
        }
        if (!open) {
            return make_database_error(jb::core::ErrorCategory::Unavailable,
                                       "db.database_closed",
                                       "The database is closed");
        }
        if (!owner_thread || *owner_thread != std::this_thread::get_id()) {
            return make_database_error(jb::core::ErrorCategory::Internal,
                                       "db.wrong_thread",
                                       "The database operation was requested from a different thread");
        }
        if (poisoned) {
            return make_database_error(jb::core::ErrorCategory::Internal,
                                       "db.connection_failed",
                                       "The database connection is unusable after an unrecoverable failure");
        }
        return std::nullopt;
    }

    template <typename T = void>
    [[nodiscard]] auto failure(jb::core::Error error) -> jb::core::Result<T, jb::core::Error>
    {
        last_error = error;
        return jb::core::Result<T, jb::core::Error>::failure(std::move(error));
    }

    std::unique_ptr<Driver>        driver;
    std::optional<jb::core::Error> last_error;
    std::optional<std::thread::id> owner_thread;
    std::size_t                    query_count{0};
    TransactionOwner               transaction_owner{TransactionOwner::None};
    std::uint64_t                  transaction_token{0};
    std::uint64_t                  next_transaction_token{1};
    bool                           open{false};
    bool                           poisoned{false};
};

} // namespace jb::db
