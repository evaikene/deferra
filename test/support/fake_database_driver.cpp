#include "fake_database_driver.hpp"

namespace jb::test {

namespace {

class FakeDriverQuery final : public jb::db::DriverQuery {
public:
    FakeDriverQuery(std::shared_ptr<FakeDatabaseDriverState> state, std::optional<std::size_t> plan_index)
        : _state{std::move(state)}
        , _plan_index{plan_index}
    {}

private:
    std::shared_ptr<FakeDatabaseDriverState> _state;
    std::optional<std::size_t>               _plan_index;
    std::size_t                              _next_record_index{0};

    [[nodiscard]] auto plan() const noexcept -> FakeDatabaseQueryPlan const*
    {
        return _plan_index ? &_state->query_plans[*_plan_index] : nullptr;
    }

    [[nodiscard]] auto prepare(std::string_view sql) -> jb::core::Result<void, jb::core::Error> override
    {
        _state->calls.emplace_back("query.prepare");
        auto const& error = plan() ? plan()->prepare_error : _state->prepare_error;
        if (error) {
            return jb::core::Result<void, jb::core::Error>::failure(*error);
        }
        _state->prepared_sql      = sql;
        _state->next_record_index = 0;
        _next_record_index        = 0;
        return jb::core::Result<void, jb::core::Error>::success();
    }

    [[nodiscard]] auto parameter_count() const noexcept -> std::size_t override
    {
        return plan() ? plan()->parameter_names.size() : _state->parameter_names.size();
    }

    [[nodiscard]] auto parameter_name(std::size_t index) const -> std::string_view override
    {
        return plan() ? plan()->parameter_names.at(index) : _state->parameter_names.at(index);
    }

    [[nodiscard]] auto bind(std::size_t index, jb::db::Value const& value)
        -> jb::core::Result<void, jb::core::Error> override
    {
        _state->calls.emplace_back("query.bind");
        auto const& error = plan() ? plan()->bind_error : _state->bind_error;
        if (error) {
            return jb::core::Result<void, jb::core::Error>::failure(*error);
        }
        _state->bindings.emplace_back(index, value);
        return jb::core::Result<void, jb::core::Error>::success();
    }

    [[nodiscard]] auto exec() -> jb::core::Result<jb::db::ExecutionInfo, jb::core::Error> override
    {
        _state->calls.emplace_back("query.exec");
        auto const& error = plan() ? plan()->exec_error : _state->exec_error;
        if (error) {
            return jb::core::Result<jb::db::ExecutionInfo, jb::core::Error>::failure(*error);
        }
        _state->next_record_index = 0;
        _next_record_index        = 0;
        return jb::core::Result<jb::db::ExecutionInfo, jb::core::Error>::success(plan() ? plan()->execution_info
                                                                                        : _state->execution_info);
    }

    [[nodiscard]] auto next() -> jb::core::Result<std::optional<jb::db::Record>, jb::core::Error> override
    {
        _state->calls.emplace_back("query.next");
        auto const& error = plan() ? plan()->next_error : _state->next_error;
        if (error) {
            return jb::core::Result<std::optional<jb::db::Record>, jb::core::Error>::failure(*error);
        }
        auto const& records = plan() ? plan()->records : _state->records;
        auto&       index   = plan() ? _next_record_index : _state->next_record_index;
        if (index >= records.size()) {
            return jb::core::Result<std::optional<jb::db::Record>, jb::core::Error>::success(std::nullopt);
        }
        return jb::core::Result<std::optional<jb::db::Record>, jb::core::Error>::success(records[index++]);
    }

    [[nodiscard]] auto finish() -> jb::core::Result<void, jb::core::Error> override
    {
        _state->calls.emplace_back("query.finish");
        auto const& error = plan() ? plan()->finish_error : _state->finish_error;
        if (error) {
            return jb::core::Result<void, jb::core::Error>::failure(*error);
        }
        return jb::core::Result<void, jb::core::Error>::success();
    }

    void clear() noexcept override { ++_state->clear_count; }
};

} // anonymous namespace

FakeDatabaseDriver::FakeDatabaseDriver(std::shared_ptr<FakeDatabaseDriverState> state)
    : _state{std::move(state)}
{}

auto FakeDatabaseDriver::name() const noexcept -> std::string_view
{
    return "fake";
}

auto FakeDatabaseDriver::open() -> jb::core::Result<void, jb::core::Error>
{
    _state->calls.emplace_back("driver.open");
    if (_state->open_error) {
        return jb::core::Result<void, jb::core::Error>::failure(*_state->open_error);
    }
    _state->open = true;
    return jb::core::Result<void, jb::core::Error>::success();
}

auto FakeDatabaseDriver::close() -> jb::core::Result<void, jb::core::Error>
{
    _state->calls.emplace_back("driver.close");
    if (_state->close_error) {
        return jb::core::Result<void, jb::core::Error>::failure(*_state->close_error);
    }
    _state->open = false;
    return jb::core::Result<void, jb::core::Error>::success();
}

auto FakeDatabaseDriver::is_open() const noexcept -> bool
{
    return _state->open;
}

auto FakeDatabaseDriver::create_query() -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>
{
    _state->calls.emplace_back("driver.create_query");
    if (_state->create_query_error) {
        return jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>::failure(
            *_state->create_query_error);
    }
    auto plan_index = std::optional<std::size_t>{};
    if (_state->next_query_plan_index < _state->query_plans.size()) {
        plan_index = _state->next_query_plan_index++;
    }
    return jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error>::success(
        std::make_unique<FakeDriverQuery>(_state, plan_index));
}

auto FakeDatabaseDriver::begin(jb::db::TransactionMode mode) -> jb::core::Result<void, jb::core::Error>
{
    _state->calls.emplace_back("driver.begin");
    _state->last_transaction_mode = mode;
    if (_state->begin_error) {
        return jb::core::Result<void, jb::core::Error>::failure(*_state->begin_error);
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto FakeDatabaseDriver::commit() -> jb::core::Result<void, jb::core::Error>
{
    _state->calls.emplace_back("driver.commit");
    if (_state->commit_error) {
        return jb::core::Result<void, jb::core::Error>::failure(*_state->commit_error);
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

auto FakeDatabaseDriver::rollback() -> jb::core::Result<void, jb::core::Error>
{
    _state->calls.emplace_back("driver.rollback");
    if (_state->rollback_error) {
        return jb::core::Result<void, jb::core::Error>::failure(*_state->rollback_error);
    }
    return jb::core::Result<void, jb::core::Error>::success();
}

} // namespace jb::test
