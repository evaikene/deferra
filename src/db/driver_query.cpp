#include "driver_query.hpp"

namespace jb::db {

auto DriverQuery::forward_prepare(DriverQuery& query, std::string_view sql) -> jb::core::Result<void, jb::core::Error>
{
    return query.prepare(sql);
}

auto DriverQuery::forward_parameter_count(DriverQuery const& query) noexcept -> std::size_t
{
    return query.parameter_count();
}

auto DriverQuery::forward_parameter_name(DriverQuery const& query, std::size_t index) -> std::string_view
{
    return query.parameter_name(index);
}

auto DriverQuery::forward_bind(DriverQuery& query, std::size_t index, Value const& value)
    -> jb::core::Result<void, jb::core::Error>
{
    return query.bind(index, value);
}

auto DriverQuery::forward_exec(DriverQuery& query) -> jb::core::Result<ExecutionInfo, jb::core::Error>
{
    return query.exec();
}

auto DriverQuery::forward_next(DriverQuery& query) -> jb::core::Result<std::optional<Record>, jb::core::Error>
{
    return query.next();
}

auto DriverQuery::forward_finish(DriverQuery& query) -> jb::core::Result<void, jb::core::Error>
{
    return query.finish();
}

auto DriverQuery::forward_clear(DriverQuery& query) noexcept -> void
{
    query.clear();
}

} // namespace jb::db
