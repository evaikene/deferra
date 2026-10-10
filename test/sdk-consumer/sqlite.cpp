#include <jb/db/database.hpp>
#include <jb/db/sqlite/sqlite_driver.hpp>
#include <jb/jobu/sqlite/sqlite_schema.hpp>

#include <memory>

int main()
{
    // A closed driver exercises both archives and their SQLite link dependency
    // without leaving a database, sidecars, or locks in the consumer directory.
    jb::db::Database database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{})};
    auto             schema = jb::jobu::sqlite::ensure_schema(database);
    return database.driver_name() == "sqlite" && !schema ? 0 : 1;
}
