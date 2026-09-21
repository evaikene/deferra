#include "fault_database_driver.hpp"

#include "driver_query.hpp"

#include <cstddef>
#include <utility>

namespace jb::test {

namespace {

template <typename Forward>
auto invoke(DatabaseFaultState& state, std::string_view boundary, DatabaseOperation operation, Forward forward)
    -> decltype(forward())
{
    using Result = decltype(forward());
    if (auto error = state.checkpoint(boundary, operation, DatabaseFaultPhase::Before)) {
        return Result::failure(std::move(*error));
    }

    auto result = forward();
    // Never replace a real backend error with a synthetic acknowledgement failure.
    if (result) {
        if (auto error = state.checkpoint(boundary, operation, DatabaseFaultPhase::AfterSuccess)) {
            return Result::failure(std::move(*error));
        }
    }
    return result;
}

class FaultDriverQuery final : public db::DriverQuery {
public:
    FaultDriverQuery(std::unique_ptr<db::DriverQuery> query, std::shared_ptr<DatabaseFaultState> state)
        : _query{std::move(query)}
        , _state{std::move(state)}
    {}

private:
    [[nodiscard]] auto prepare(std::string_view sql) -> core::Result<void, core::Error> override
    {
        _boundary = _state->classify ? _state->classify(sql) : "statement";
        return invoke(*_state, _boundary, DatabaseOperation::Prepare, [&] { return forward_prepare(*_query, sql); });
    }

    [[nodiscard]] auto parameter_count() const noexcept -> std::size_t override
    {
        return forward_parameter_count(*_query);
    }

    [[nodiscard]] auto parameter_name(std::size_t index) const -> std::string_view override
    {
        return forward_parameter_name(*_query, index);
    }

    [[nodiscard]] auto bind(std::size_t index, db::Value const& value) -> core::Result<void, core::Error> override
    {
        return invoke(*_state, _boundary, DatabaseOperation::Bind, [&] { return forward_bind(*_query, index, value); });
    }

    [[nodiscard]] auto exec() -> core::Result<db::ExecutionInfo, core::Error> override
    {
        return invoke(*_state, _boundary, DatabaseOperation::Execute, [&] { return forward_exec(*_query); });
    }

    [[nodiscard]] auto next() -> core::Result<std::optional<db::Record>, core::Error> override
    {
        return invoke(*_state, _boundary, DatabaseOperation::Fetch, [&] { return forward_next(*_query); });
    }

    [[nodiscard]] auto finish() -> core::Result<void, core::Error> override
    {
        return invoke(*_state, _boundary, DatabaseOperation::Finish, [&] { return forward_finish(*_query); });
    }

    // Cleanup and metadata access remain transparent and do not allocate tracing entries in noexcept operations.
    void clear() noexcept override { forward_clear(*_query); }

    std::unique_ptr<db::DriverQuery>    _query;
    std::shared_ptr<DatabaseFaultState> _state;
    std::string                         _boundary;
};

} // namespace

auto DatabaseFaultState::checkpoint(std::string_view boundary, DatabaseOperation operation, DatabaseFaultPhase phase)
    -> std::optional<core::Error>
{
    auto call = DatabaseCall{.boundary = std::string{boundary}, .operation = operation, .phase = phase};
    calls.push_back(call);
    for (auto& fault : faults) {
        if (!fault.fired && fault.at == call) {
            fault.fired = true;
            return fault.error;
        }
    }
    return std::nullopt;
}

FaultDatabaseDriver::FaultDatabaseDriver(std::unique_ptr<db::Driver> driver, std::shared_ptr<DatabaseFaultState> state)
    : _driver{std::move(driver)}
    , _state{std::move(state)}
{}

auto FaultDatabaseDriver::name() const noexcept -> std::string_view
{
    return _driver->name();
}

auto FaultDatabaseDriver::is_open() const noexcept -> bool
{
    return forward_is_open(*_driver);
}

auto FaultDatabaseDriver::open() -> core::Result<void, core::Error>
{
    return invoke(*_state, "connection", DatabaseOperation::Open, [&] { return forward_open(*_driver); });
}

auto FaultDatabaseDriver::close() -> core::Result<void, core::Error>
{
    return invoke(*_state, "connection", DatabaseOperation::Close, [&] { return forward_close(*_driver); });
}

auto FaultDatabaseDriver::create_query() -> core::Result<std::unique_ptr<db::DriverQuery>, core::Error>
{
    return invoke(*_state,
                  "connection",
                  DatabaseOperation::CreateQuery,
                  [&]() -> core::Result<std::unique_ptr<db::DriverQuery>, core::Error> {
                      auto query = forward_create_query(*_driver);
                      if (!query) {
                          return core::Result<std::unique_ptr<db::DriverQuery>, core::Error>::failure(
                              std::move(query).error());
                      }
                      return core::Result<std::unique_ptr<db::DriverQuery>, core::Error>::success(
                          std::make_unique<FaultDriverQuery>(std::move(*query), _state));
                  });
}

auto FaultDatabaseDriver::begin(db::TransactionMode mode) -> core::Result<void, core::Error>
{
    return invoke(*_state, "connection", DatabaseOperation::Begin, [&] { return forward_begin(*_driver, mode); });
}

auto FaultDatabaseDriver::commit() -> core::Result<void, core::Error>
{
    return invoke(*_state, "connection", DatabaseOperation::Commit, [&] { return forward_commit(*_driver); });
}

auto FaultDatabaseDriver::rollback() -> core::Result<void, core::Error>
{
    return invoke(*_state, "connection", DatabaseOperation::Rollback, [&] { return forward_rollback(*_driver); });
}

} // namespace jb::test
