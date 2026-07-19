#include "record.hpp"

#include <utility>
#include <variant>

namespace jb::db {

namespace {

constexpr auto ascii_lower(char value) noexcept -> char
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_case_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

Field::Field(std::string name, Value value)
    : _name{std::move(name)}
    , _value{std::move(value)}
{}

auto Field::name() const noexcept -> std::string const&
{
    return _name;
}

auto Field::value() const noexcept -> Value const&
{
    return _value;
}

auto Field::is_null() const noexcept -> bool
{
    return std::holds_alternative<Null>(_value);
}

Record::Record(std::vector<Field> fields)
    : _fields{std::move(fields)}
{}

auto Record::count() const noexcept -> std::size_t
{
    return _fields.size();
}

auto Record::is_empty() const noexcept -> bool
{
    return _fields.empty();
}

auto Record::contains(std::string_view name) const noexcept -> bool
{
    return index_of(name) >= 0;
}

auto Record::index_of(std::string_view name) const noexcept -> int
{
    for (std::size_t index = 0; index < _fields.size(); ++index) {
        if (ascii_case_equal(_fields[index].name(), name)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

auto Record::field(std::size_t index) const -> Field const&
{
    return _fields.at(index);
}

auto Record::field_name(std::size_t index) const -> std::string const&
{
    return field(index).name();
}

auto Record::value(std::size_t index) const -> Value const&
{
    return field(index).value();
}

auto Record::value(std::string_view name) const -> Value const*
{
    auto const index = index_of(name);
    return index < 0 ? nullptr : &_fields[static_cast<std::size_t>(index)].value();
}

auto Record::is_null(std::size_t index) const noexcept -> bool
{
    return index >= _fields.size() || _fields[index].is_null();
}

auto Record::is_null(std::string_view name) const noexcept -> bool
{
    auto const* field_value = value(name);
    return field_value == nullptr || std::holds_alternative<Null>(*field_value);
}

} // namespace jb::db
