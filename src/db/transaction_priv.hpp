#pragma once

#include "transaction.hpp"

namespace jb::db::detail {

struct TransactionAccess {
    static void replace_token(Transaction& transaction, std::uint64_t token) noexcept { transaction._token = token; }
};

} // namespace jb::db::detail
