#pragma once

#include "error.hpp"

#include <sqlite3.h>

#include <string_view>

namespace jb::db::sqlite::detail {

[[nodiscard]] auto make_sqlite_error(sqlite3*                connection,
                                     int                     result_code,
                                     std::string_view        fallback_code,
                                     jb::core::ErrorCategory fallback_category,
                                     std::string_view        message) -> jb::core::Error;

} // namespace jb::db::sqlite::detail
