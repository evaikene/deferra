#include <phpcxx/arguments.hpp>
#include <phpcxx/array.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <ext/date/php_date.h>
}

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

// Ensure even an aborted Catch assertion cannot contaminate the next test's
// Zend exception state. Values in each test die before this guard clears it.
struct ExceptionScope {
    ExceptionScope() { REQUIRE_FALSE(php::has_exception()); }

    ~ExceptionScope()
    {
        if (php::has_exception()) {
            zend_clear_exception();
        }
    }
};

auto evaluate(std::string_view expression) -> php::Value
{
    zval result{};
    ZVAL_UNDEF(&result);
    auto const status = zend_eval_stringl(expression.data(), expression.size(), &result, "php-cxx conversion test");
    auto       owner  = php::Value::adopt(result);
    REQUIRE(status == SUCCESS);
    REQUIRE_FALSE(php::has_exception());
    return owner;
}

auto check_php(std::string_view expression) -> void
{
    auto result = evaluate(expression);
    REQUIRE(result.view().is_true());
}

auto fails_with(std::string_view expression, std::string_view class_name) -> void
{
    // Catch inside PHP so an expected engine error never escapes into Catch2.
    auto code   = "(function () { try { " + std::string(expression) + "; } catch (\\" + std::string(class_name) +
                  " $e) { return true; } return false; })()";
    auto result = evaluate(code);
    REQUIRE(result.view().is_true());
}

} // namespace

TEST_CASE("Results carry values, move-only owners, and owned errors", "[php-cxx][conversion]")
{
    static_assert(!std::is_copy_constructible_v<php::Result<php::Value>>);
    php::Result<int> number(7);
    REQUIRE(number);
    CHECK(number.value() == 7);
    auto const copied = number;
    CHECK(copied.value() == 7);

    auto                     string = php::String::create("owned");
    auto const*              raw    = string.get();
    php::Result<php::String> result(std::move(string));
    CHECK_FALSE(string);
    auto extracted = std::move(result).value();
    CHECK(extracted.get() == raw);

    php::Result<void> success;
    REQUIRE(success);
    php::Result<void> failure(php::Error{.kind = php::ErrorKind::Value, .message = "range", .argument = 2});
    REQUIRE_FALSE(failure);
    CHECK(failure.error().argument == 2);
    auto             error = std::move(failure).error();
    php::Result<int> failed(std::move(error));
    CHECK(failed.error().message == "range");
    CHECK(std::move(failed).error().kind == php::ErrorKind::Value);
}

TEST_CASE("Checked extraction preserves exact types, references, and borrowed lifetimes", "[php-cxx][conversion]")
{
    ExceptionScope const scope;
    auto                 values = evaluate(R"([true, 42, 1.5, "a\0b", [], null])");
    auto const           array  = values.view().get_array();
    CHECK(php::as_bool(array[0]).value());
    CHECK(php::as_integer(array[1]).value() == 42);
    CHECK(php::as_double(array[2]).value() == 1.5);
    CHECK(php::as_string(array[3]).value().bytes() == std::string_view("a\0b", 3));
    CHECK(php::as_array(array[4]).value().empty());
    CHECK_FALSE(php::as_double(array[1]));
    CHECK_FALSE(php::as_integer(array[2]));
    CHECK_FALSE(php::as_bool(array[1]));
    CHECK_FALSE(php::as_array(array[3]));
    CHECK_FALSE(php::as_string(array[5]));
    CHECK_FALSE(php::as_integer(php::ValueView{}));
    php::Value undefined;
    CHECK_FALSE(php::as_integer(undefined.view()));

    auto       references = evaluate("(function () { $v = 'ref'; return [&$v, &$v]; })()");
    auto const reference  = references.view().get_array()[0];
    REQUIRE(reference.is_reference());
    auto const before   = GC_REFCOUNT(reference.get()->value.ref);
    auto       borrowed = php::as_string(reference);
    REQUIRE(borrowed);
    CHECK(borrowed.value() == "ref");
    CHECK(GC_REFCOUNT(reference.get()->value.ref) == before);
    CHECK(reference.is_reference());
    auto retained = php::String::retain(borrowed.value());
    references    = php::Value::null();
    CHECK(retained.view() == "ref");
}

TEST_CASE("Numeric conversions reject overflow, signedness loss, and fractional values", "[php-cxx][conversion]")
{
    CHECK(php::checked_integer<std::int8_t>(-128).value() == -128);
    CHECK(php::checked_integer<std::int8_t>(127).value() == 127);
    CHECK_FALSE(php::checked_integer<std::int8_t>(128));
    CHECK_FALSE(php::checked_integer<std::int8_t>(-129));
    CHECK_FALSE(php::checked_integer<unsigned>(-1));
    CHECK(php::checked_integer<unsigned char>(255).value() == 255);
    CHECK_FALSE(php::checked_integer<unsigned char>(256));
    CHECK(php::checked_integer<std::uint64_t>(ZEND_LONG_MAX).value() == static_cast<std::uint64_t>(ZEND_LONG_MAX));
    CHECK(php::checked_integer<std::int64_t>(ZEND_LONG_MIN).value() == ZEND_LONG_MIN);
    CHECK(php::integer_from_double(-9223372036854775808.0).value() == ZEND_LONG_MIN);
    CHECK_FALSE(php::integer_from_double(9223372036854775808.0));
    CHECK_FALSE(php::integer_from_double(std::nextafter(-9223372036854775808.0, -INFINITY)));
    CHECK(php::integer_from_double(std::nextafter(9223372036854775808.0, 0.0)));
    CHECK_FALSE(php::integer_from_double(1.5));
    CHECK_FALSE(php::integer_from_double(std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(php::integer_from_double(std::numeric_limits<double>::infinity()));
    CHECK(php::integer_from_double(-0.0).value() == 0);
    CHECK(php::double_from_integer(42) == 42.0);
}

TEST_CASE("Arguments honor weak callers, missing/null policy, and PHP references", "[php-cxx][conversion]")
{
    ExceptionScope const scope;
    check_php("phpcxx_probe('integer', '42') === 42");
    check_php("phpcxx_probe('float', 42) === 42.0");
    check_php("phpcxx_probe('boolean', '0') === false");
    check_php("phpcxx_probe('string', 42) === '42'");
    check_php("phpcxx_probe('array', [1,2]) === 2");
    check_php("phpcxx_probe('object', new DateTimeImmutable()) === true");
    check_php("phpcxx_probe('optional') === 'missing'");
    check_php("phpcxx_probe(value: null, operation: 'optional') === 'null'");
    check_php("phpcxx_probe(value: 17, operation: 'integer') === 17");
    check_php("(function () { $x = '23'; $ref =& $x; return phpcxx_probe('integer', $ref); })() === 23");
    fails_with("phpcxx_probe('integer')", "ArgumentCountError");
    fails_with("phpcxx_probe('integer', 1, 2)", "ArgumentCountError");
    fails_with("phpcxx_probe('integer', null)", "TypeError");
    fails_with("phpcxx_probe('integer', [])", "TypeError");
    fails_with("phpcxx_probe('array', 1)", "TypeError");
    fails_with("phpcxx_probe('object', new stdClass())", "TypeError");
    fails_with("phpcxx_probe('range', 256)", "ValueError");

    // A string conversion may execute user code. Keep its original exception.
    auto preserved =
        evaluate("(function () { $original = new RuntimeException('original'); "
                 "$x = new class($original) { public function __construct(private $e) {} "
                 "public function __toString(): string { throw $this->e; } }; "
                 "try { phpcxx_probe('string', $x); } catch (Throwable $e) { return $e === $original; } })()");
    CHECK(preserved.view().is_true());
}

TEST_CASE("Class checks accept interfaces and subclasses without retaining objects", "[php-cxx][conversion]")
{
    ExceptionScope const scope;
    auto                 object = evaluate("new class('2000-01-01 UTC') extends DateTimeImmutable {}");
    auto const           before = GC_REFCOUNT(object.view().get_object());
    auto                 result = php::as_object(object.view(), php_date_get_interface_ce());
    REQUIRE(result);
    CHECK(result.value() == object.view().get_object());
    CHECK(GC_REFCOUNT(result.value()) == before);
    CHECK_FALSE(php::as_object(object.view(), php_date_get_date_ce()));
    CHECK_FALSE(php::as_object(php::ValueView{}, php_date_get_interface_ce()));
}

TEST_CASE("Native dates roundtrip microseconds and normalize output to UTC", "[php-cxx][conversion]")
{
    ExceptionScope const scope;
    auto                 input   = evaluate("new DateTime('1970-01-01 01:59:59.500001 +02:00')");
    auto                 instant = php::as_timestamp(input.view());
    REQUIRE(instant);
    CHECK(instant.value().seconds == -1);
    CHECK(instant.value().microseconds == 500001);

    for (auto const micros : {std::int64_t{0},
                              std::int64_t{-1},
                              std::int64_t{1},
                              std::int64_t{-500001},
                              std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()}) {
        auto const timestamp = php::timestamp_from_microseconds(micros);
        CHECK(php::unix_microseconds(timestamp).value() == micros);
        auto date = php::datetime_immutable(timestamp);
        REQUIRE(date);
        CHECK(date.value().view().get_object()->ce == php_date_get_immutable_ce());
        auto roundtrip = php::as_timestamp(date.value().view());
        REQUIRE(roundtrip);
        CHECK(php::unix_microseconds(roundtrip.value()).value() == micros);
    }
    CHECK_FALSE(php::datetime_immutable({.seconds = 0, .microseconds = -1}));
    CHECK_FALSE(php::unix_microseconds({.seconds = 0, .microseconds = 1000000}));
    auto lower = php::timestamp_from_microseconds(std::numeric_limits<std::int64_t>::min());
    --lower.microseconds;
    CHECK_FALSE(php::unix_microseconds(lower));
    auto upper = php::timestamp_from_microseconds(std::numeric_limits<std::int64_t>::max());
    ++upper.microseconds;
    CHECK_FALSE(php::unix_microseconds(upper));

    auto uninitialized = evaluate("new class extends DateTime { public function __construct() {} }");
    auto failed        = php::as_timestamp(uninitialized.view());
    REQUIRE_FALSE(failed);
    CHECK(failed.error().kind == php::ErrorKind::Value);
    CHECK_FALSE(php::has_exception());
}

TEST_CASE("PHP exception helpers preserve the original pending exception", "[php-cxx][conversion]")
{
    ExceptionScope const scope;
    fails_with("phpcxx_probe('raise', 'type')", "TypeError");
    fails_with("phpcxx_probe('raise', 'value')", "ValueError");
    fails_with("phpcxx_probe('raise', 'count')", "ArgumentCountError");
    fails_with("phpcxx_probe('raise', 'runtime')", "Exception");
    fails_with("phpcxx_probe('raise', 'custom')", "Error");
    auto preserved = evaluate("phpcxx_probe('pending')");
    CHECK(preserved.view().is_true());
    CHECK_FALSE(php::has_exception());
}
