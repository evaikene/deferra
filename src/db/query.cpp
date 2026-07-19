#include "query.hpp"

#include "database_priv.hpp"
#include "query_priv.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace jb::db {

namespace {

using QueryResult = jb::core::Result<void, jb::core::Error>;

template <typename T>
auto failure(std::optional<jb::core::Error>& last_error, jb::core::Error error) -> jb::core::Result<T, jb::core::Error>
{
    last_error = error;
    return jb::core::Result<T, jb::core::Error>::failure(std::move(error));
}

template <typename T>
auto untracked_failure(jb::core::Error error) -> jb::core::Result<T, jb::core::Error>
{
    return jb::core::Result<T, jb::core::Error>::failure(std::move(error));
}

auto invalid_query_error(std::string_view message) -> jb::core::Error
{
    return make_database_error(jb::core::ErrorCategory::InvalidArgument, "db.invalid_query", message);
}

auto invalid_parameter_error(std::string_view message) -> jb::core::Error
{
    return make_database_error(jb::core::ErrorCategory::InvalidArgument, "db.invalid_parameter", message);
}

} // anonymous namespace

Query::Query(Database& database)
    : _data{std::make_unique<Private>(database)}
{
    if (!database._data) {
        database._data = std::make_unique<Database::Private>();
    }
    ++database._data->query_count;
}

Query::~Query()
{
    if (!_data) {
        return;
    }
    auto* database = _data->database;
    clear();
    if (database && database->_data && database->_data->query_count != 0) {
        --database->_data->query_count;
    }
}

Query::Query(Query&& other) noexcept
    : _data{std::move(other._data)}
{}

auto Query::prepare(std::string_view sql) -> QueryResult
{
    if (!_data) {
        return untracked_failure<void>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }
    if (sql.empty()) {
        return failure<void>(_data->last_error, invalid_query_error("The SQL statement is empty"));
    }
    if (_data->active) {
        auto finished = finish();
        if (!finished) {
            return finished;
        }
    }

    if (_data->driver_query) {
        _data->driver_query->clear();
        _data->driver_query.reset();
    }
    _data->sql.clear();
    _data->parameter_names.clear();
    _data->bindings.clear();
    _data->binding_mode       = BindingMode::None;
    _data->next_bind_position = 0;
    _data->reset_execution();

    auto created = _data->database->_data->driver->create_query();
    if (!created) {
        return failure<void>(_data->last_error, std::move(created).error());
    }
    _data->driver_query = std::move(created).value();

    auto prepared = _data->driver_query->prepare(sql);
    if (!prepared) {
        _data->driver_query->clear();
        _data->driver_query.reset();
        return failure<void>(_data->last_error, std::move(prepared).error());
    }

    _data->sql       = sql;
    auto const count = _data->driver_query->parameter_count();
    _data->parameter_names.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        _data->parameter_names.emplace_back(_data->driver_query->parameter_name(index));
    }
    _data->bindings.resize(count);
    _data->last_error.reset();
    return QueryResult::success();
}

auto Query::exec() -> QueryResult
{
    if (!_data) {
        return untracked_failure<void>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }
    if (!_data->driver_query || _data->sql.empty()) {
        return failure<void>(_data->last_error, invalid_query_error("The query has no prepared statement"));
    }
    if (_data->active) {
        auto finished = finish();
        if (!finished) {
            return finished;
        }
    }
    if (std::any_of(_data->bindings.begin(), _data->bindings.end(), [](auto const& value) { return !value; })) {
        return failure<void>(_data->last_error,
                             make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                 "db.parameter_unbound",
                                                 "Every query parameter must be explicitly bound"));
    }

    auto executed = _data->driver_query->exec();
    if (!executed) {
        _data->reset_execution();
        return failure<void>(_data->last_error, std::move(executed).error());
    }

    auto info              = std::move(executed).value();
    _data->active          = true;
    _data->valid           = false;
    _data->select          = info.produces_records;
    _data->at_end          = false;
    _data->rows_affected   = info.rows_affected;
    _data->record_metadata = std::move(info.record_metadata);
    _data->current_record  = _data->select ? _data->record_metadata : Record{};
    _data->last_error.reset();
    return QueryResult::success();
}

auto Query::exec(std::string_view sql) -> QueryResult
{
    auto prepared = prepare(sql);
    if (!prepared) {
        return prepared;
    }
    return exec();
}

auto Query::bind_value(std::size_t position, Value value) -> QueryResult
{
    if (!_data) {
        return untracked_failure<void>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }
    if (!_data->driver_query || _data->sql.empty()) {
        return failure<void>(_data->last_error, invalid_query_error("The query has no prepared statement"));
    }
    if (_data->binding_mode == BindingMode::Named) {
        return failure<void>(_data->last_error,
                             make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                 "db.mixed_binding",
                                                 "Named and positional binding cannot be mixed"));
    }
    if (position >= _data->bindings.size()) {
        return failure<void>(_data->last_error, invalid_parameter_error("The parameter position is out of range"));
    }
    if (_data->active) {
        auto finished = finish();
        if (!finished) {
            return finished;
        }
    }

    _data->binding_mode       = BindingMode::Positional;
    _data->bindings[position] = std::move(value);
    auto bound                = _data->driver_query->bind(position, *_data->bindings[position]);
    if (!bound) {
        _data->bindings[position].reset();
        return failure<void>(_data->last_error, std::move(bound).error());
    }
    _data->last_error.reset();
    return QueryResult::success();
}

auto Query::bind_value(std::string_view placeholder, Value value) -> QueryResult
{
    if (!_data) {
        return untracked_failure<void>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }
    if (!_data->driver_query || _data->sql.empty()) {
        return failure<void>(_data->last_error, invalid_query_error("The query has no prepared statement"));
    }
    if (_data->binding_mode == BindingMode::Positional) {
        return failure<void>(_data->last_error,
                             make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                 "db.mixed_binding",
                                                 "Named and positional binding cannot be mixed"));
    }
    if (placeholder.empty()) {
        return failure<void>(_data->last_error,
                             invalid_parameter_error("A named parameter must include its placeholder prefix"));
    }

    auto const parameter = std::find(_data->parameter_names.begin(), _data->parameter_names.end(), placeholder);
    if (parameter == _data->parameter_names.end()) {
        return failure<void>(_data->last_error, invalid_parameter_error("The named parameter does not exist"));
    }
    if (_data->active) {
        auto finished = finish();
        if (!finished) {
            return finished;
        }
    }

    auto const position       = static_cast<std::size_t>(parameter - _data->parameter_names.begin());
    _data->binding_mode       = BindingMode::Named;
    _data->bindings[position] = std::move(value);
    auto bound                = _data->driver_query->bind(position, *_data->bindings[position]);
    if (!bound) {
        _data->bindings[position].reset();
        return failure<void>(_data->last_error, std::move(bound).error());
    }
    _data->last_error.reset();
    return QueryResult::success();
}

auto Query::add_bind_value(Value value) -> QueryResult
{
    if (!_data) {
        return untracked_failure<void>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }
    if (!_data->driver_query || _data->sql.empty()) {
        return failure<void>(_data->last_error, invalid_query_error("The query has no prepared statement"));
    }
    if (_data->binding_mode == BindingMode::Named) {
        return failure<void>(_data->last_error,
                             make_database_error(jb::core::ErrorCategory::InvalidArgument,
                                                 "db.mixed_binding",
                                                 "Named and positional binding cannot be mixed"));
    }
    auto position = _data->next_bind_position;
    while (position < _data->bindings.size() && _data->bindings[position]) {
        ++position;
    }
    if (position >= _data->bindings.size()) {
        return failure<void>(_data->last_error, invalid_parameter_error("The query has no unbound parameter"));
    }

    auto bound = bind_value(position, std::move(value));
    if (bound) {
        _data->next_bind_position = position + 1;
    }
    return bound;
}

auto Query::next() -> jb::core::Result<bool, jb::core::Error>
{
    if (!_data) {
        return untracked_failure<bool>(invalid_query_error("The query has been moved from"));
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<bool>(_data->last_error, std::move(*error));
    }
    if (!_data->driver_query || !_data->active || !_data->select) {
        return failure<bool>(_data->last_error, invalid_query_error("The query has no active record cursor"));
    }
    if (_data->at_end) {
        _data->last_error.reset();
        return jb::core::Result<bool, jb::core::Error>::success(false);
    }

    auto next_record = _data->driver_query->next();
    if (!next_record) {
        _data->valid          = false;
        _data->current_record = _data->record_metadata;
        return failure<bool>(_data->last_error, std::move(next_record).error());
    }
    if (!next_record.value()) {
        _data->valid          = false;
        _data->at_end         = true;
        _data->current_record = _data->record_metadata;
        _data->last_error.reset();
        return jb::core::Result<bool, jb::core::Error>::success(false);
    }

    _data->valid          = true;
    _data->at_end         = false;
    _data->current_record = std::move(*next_record.value());
    _data->last_error.reset();
    return jb::core::Result<bool, jb::core::Error>::success(true);
}

auto Query::is_active() const noexcept -> bool
{
    return _data && _data->active;
}

auto Query::is_valid() const noexcept -> bool
{
    return _data && _data->valid;
}

auto Query::is_select() const noexcept -> bool
{
    return _data && _data->active && _data->select;
}

auto Query::record() const -> Record const&
{
    if (!_data || !_data->active || !_data->select) {
        throw std::logic_error{"Query does not have an active record cursor"};
    }
    return _data->current_record;
}

auto Query::value(std::size_t index) const -> Value const&
{
    if (!_data || !_data->valid) {
        throw std::logic_error{"Query is not positioned on a record"};
    }
    return _data->current_record.value(index);
}

auto Query::value(std::string_view name) const -> Value const*
{
    return _data && _data->valid ? _data->current_record.value(name) : nullptr;
}

auto Query::is_null(std::size_t index) const noexcept -> bool
{
    return !_data || !_data->valid || _data->current_record.is_null(index);
}

auto Query::is_null(std::string_view name) const noexcept -> bool
{
    return !_data || !_data->valid || _data->current_record.is_null(name);
}

auto Query::num_rows_affected() const noexcept -> std::int64_t
{
    return _data && _data->active ? _data->rows_affected : -1;
}

auto Query::last_query() const noexcept -> std::string_view
{
    return _data ? std::string_view{_data->sql} : std::string_view{};
}

auto Query::last_error() const -> std::optional<jb::core::Error>
{
    return _data ? _data->last_error : std::nullopt;
}

auto Query::finish() -> QueryResult
{
    if (!_data || !_data->active) {
        if (_data) {
            _data->last_error.reset();
        }
        return QueryResult::success();
    }
    if (auto error = _data->database->_data->operation_error()) {
        return failure<void>(_data->last_error, std::move(*error));
    }

    auto finished = _data->driver_query->finish();
    if (!finished) {
        return failure<void>(_data->last_error, std::move(finished).error());
    }
    _data->reset_execution();
    _data->last_error.reset();
    return QueryResult::success();
}

void Query::clear() noexcept
{
    if (!_data) {
        return;
    }
    if (_data->driver_query) {
        _data->driver_query->clear();
        _data->driver_query.reset();
    }
    _data->sql.clear();
    _data->parameter_names.clear();
    _data->bindings.clear();
    _data->binding_mode       = BindingMode::None;
    _data->next_bind_position = 0;
    _data->reset_execution();
    _data->last_error.reset();
}

} // namespace jb::db
