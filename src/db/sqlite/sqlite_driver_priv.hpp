#pragma once

#include "sqlite_driver.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <utility>

namespace jb::db::sqlite {

struct Driver::Private {
    explicit Private(Options value)
        : options{std::move(value)}
    {}

    Options               options;
    std::filesystem::path database_path;
    std::filesystem::path lock_path;
    sqlite3*              connection{nullptr};
    int                   lock_fd{-1};
};

} // namespace jb::db::sqlite
