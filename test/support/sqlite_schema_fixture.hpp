/// @file sqlite_schema_fixture.hpp
/// @brief Creates the original JobU schema for upgrade and startup tests.
#pragma once

#include "database.hpp"
#include "query.hpp"
#include "sqlite/sqlite_schema_priv.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

namespace jb::test {

/// Builds genuine version-1 objects in an empty database, without creating or downgrading version 2.
inline void create_version_one_schema(db::Database& database)
{
    auto transaction = db::Transaction::begin(database);
    REQUIRE(transaction);
    for (auto const& object : jobu::sqlite::detail::schema_v1_object_manifest()) {
        db::Query query{database};
        REQUIRE(query.exec(object.ddl));
    }
    {
        db::Query marker{database};
        REQUIRE(marker.exec("INSERT INTO jobu_schema(singleton, version) VALUES (1, 1)"));
    }
    REQUIRE(transaction->commit());
}

} // namespace jb::test
