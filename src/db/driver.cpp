#include "driver.hpp"

#include "driver_query.hpp"

namespace jb::db {

auto Driver::forward_open(Driver& driver) -> jb::core::Result<void, jb::core::Error>
{
    return driver.open();
}

auto Driver::forward_close(Driver& driver) -> jb::core::Result<void, jb::core::Error>
{
    return driver.close();
}

auto Driver::forward_is_open(Driver const& driver) noexcept -> bool
{
    return driver.is_open();
}

auto Driver::forward_create_query(Driver& driver) -> jb::core::Result<std::unique_ptr<DriverQuery>, jb::core::Error>
{
    return driver.create_query();
}

auto Driver::forward_begin(Driver& driver, TransactionMode mode) -> jb::core::Result<void, jb::core::Error>
{
    return driver.begin(mode);
}

auto Driver::forward_commit(Driver& driver) -> jb::core::Result<void, jb::core::Error>
{
    return driver.commit();
}

auto Driver::forward_rollback(Driver& driver) -> jb::core::Result<void, jb::core::Error>
{
    return driver.rollback();
}

} // namespace jb::db
