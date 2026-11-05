#include <phpcxx/array.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/views.hpp>
#include <phpcxx/zend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

auto insert_integer(zend_array* array, zend_long key, zend_long number) -> void
{
    zval value{};
    ZVAL_LONG(&value, number);
    REQUIRE(zend_hash_index_update(array, static_cast<zend_ulong>(key), &value) != nullptr);
}

auto insert_integer(zend_array* array, std::string_view key, zend_long number) -> void
{
    zval value{};
    ZVAL_LONG(&value, number);
    REQUIRE(zend_symtable_str_update(array, key.data(), key.size(), &value) != nullptr);
}

} // namespace

TEST_CASE("String views expose zero-terminated storage and read-only character iteration", "[php-cxx][views]")
{
    constexpr std::string_view bytes("ab\0CdAb", 7);
    auto                       owner = php::String::create(bytes);
    auto const                 view  = owner.view();
    auto const                 count = GC_REFCOUNT(owner.get());

    CHECK(view.data() == ZSTR_VAL(owner.get()));
    CHECK(view.c_str() == view.data());
    CHECK(view.c_str()[view.size()] == '\0');
    CHECK(view.size() == bytes.size());
    CHECK(view.length() == bytes.size());
    CHECK_FALSE(view.empty());
    CHECK(view.front() == 'a');
    CHECK(view.back() == 'b');
    CHECK(view[2] == '\0');
    CHECK(std::string(view.begin(), view.end()) == bytes);
    CHECK(std::string(view.cbegin(), view.cend()) == bytes);
    CHECK(std::string(view.rbegin(), view.rend()) == std::string(bytes.rbegin(), bytes.rend()));
    CHECK(GC_REFCOUNT(owner.get()) == count);
}

TEST_CASE("String view comparisons and searches preserve bytes after embedded NUL", "[php-cxx][views]")
{
    constexpr std::string_view bytes("ab\0CdAb", 7);
    auto                       owner = php::String::create(bytes);
    auto                       equal = php::String::create(bytes);
    auto                       part  = php::String::create(std::string_view("\0Cd", 3));
    auto const                 view  = owner.view();

    CHECK(view == equal.view());
    CHECK(view == bytes);
    CHECK(bytes == view);
    CHECK(view != "ab");
    CHECK(view.compare(equal.view()) == 0);
    CHECK(view.compare("ab") > 0);
    CHECK(view.compare("ac") < 0);
    CHECK(view.icompare(std::string_view("AB\0cDaB", 7)) == 0);
    CHECK(view.iequals(std::string_view("AB\0cDaB", 7)));
    CHECK(view.iequals(equal.view()));
    CHECK(view.icompare(equal.view()) == 0);
    CHECK(view.icompare("ab") > 0);
    CHECK(view.icompare("ac") < 0);
    CHECK_FALSE(view.iequals(std::string_view("AB\0cXaB", 7)));

    CHECK(view.starts_with("ab"));
    CHECK(view.starts_with('a'));
    CHECK_FALSE(view.starts_with('A'));
    CHECK(view.ends_with("Ab"));
    CHECK(view.ends_with('b'));
    CHECK_FALSE(view.ends_with('B'));
    CHECK(view.starts_with(equal.view()));
    CHECK(view.ends_with(equal.view()));
    CHECK(view.contains(part.view()));
    CHECK(view.contains(std::string_view("\0C", 2)));
    CHECK(view.contains('\0'));
    CHECK_FALSE(view.contains("AB"));
    CHECK_FALSE(view.contains('x'));
    CHECK(view.find(part.view()) == 2);
    CHECK(view.find(std::string_view("\0C", 2)) == 2);
    CHECK(view.find('b', 2) == 6);
    CHECK(view.find('x') == php::StringView::npos);
    CHECK(view.find("ab", view.size() + 1) == php::StringView::npos);
    CHECK(view.rfind('b') == 6);
    CHECK(view.rfind('b', 5) == 1);
    CHECK(view.rfind(part.view()) == 2);
    CHECK(view.rfind(std::string_view("\0C", 2)) == 2);
    CHECK(view.rfind("missing") == php::StringView::npos);

    // ASCII folding must leave high bytes unchanged and compare them as unsigned bytes.
    auto high = php::String::create(std::string_view("\xC0Z", 2));
    CHECK(high.view().icompare(std::string_view("\xC0z", 2)) == 0);
    CHECK(high.view().icompare(std::string_view("\xE0z", 2)) < 0);
}

TEST_CASE("String views match prefixes and suffixes with binary-safe ASCII case folding", "[php-cxx][views]")
{
    auto       owner  = php::String::create(std::string_view("Ab\0cD", 5));
    auto       prefix = php::String::create(std::string_view("aB\0C", 4));
    auto       suffix = php::String::create(std::string_view("B\0Cd", 4));
    auto const view   = owner.view();
    auto const count  = GC_REFCOUNT(owner.get());

    CHECK(view.istarts_with(prefix.view()));
    CHECK(view.iends_with(suffix.view()));
    CHECK(view.istarts_with(prefix.view().bytes()));
    CHECK(view.iends_with(suffix.view().bytes()));
    CHECK_FALSE(view.starts_with(prefix.view()));
    CHECK_FALSE(view.ends_with(suffix.view()));
    CHECK(view.istarts_with('a'));
    CHECK(view.iends_with('d'));
    CHECK_FALSE(view.istarts_with('x'));
    CHECK_FALSE(view.iends_with('x'));
    CHECK(view.istarts_with(std::string_view("aB\0Cd", 5)));
    CHECK(view.iends_with(std::string_view("aB\0Cd", 5)));
    CHECK_FALSE(view.istarts_with(std::string_view("aB\0Xd", 5)));
    CHECK_FALSE(view.iends_with(std::string_view("aB\0Xd", 5)));
    CHECK_FALSE(view.istarts_with(std::string_view("aB\0CdX", 6)));
    CHECK_FALSE(view.iends_with(std::string_view("XaB\0Cd", 6)));
    CHECK(view.istarts_with(std::string_view{}));
    CHECK(view.iends_with(std::string_view{}));
    CHECK(view.istarts_with(php::StringView{}));
    CHECK(view.iends_with(php::StringView{}));
    CHECK(GC_REFCOUNT(owner.get()) == count);

    auto binary = php::String::create(std::string_view("\0\xC0Z\xE0\0", 5));
    CHECK(binary.view().istarts_with('\0'));
    CHECK(binary.view().iends_with('\0'));
    CHECK(binary.view().istarts_with(std::string_view("\0\xC0z", 3)));
    CHECK(binary.view().iends_with(std::string_view("z\xE0\0", 3)));
    CHECK_FALSE(binary.view().istarts_with(std::string_view("\0\xE0z", 3)));
    CHECK_FALSE(binary.view().iends_with(std::string_view("z\xC0\0", 3)));
}

TEST_CASE("Absent and valid empty string views share empty byte operations", "[php-cxx][views]")
{
    auto empty = php::String::create({});
    for (auto const view : {php::StringView{}, empty.view()}) {
        CHECK(view.empty());
        CHECK(view.size() == 0);
        CHECK(view.length() == 0);
        REQUIRE(view.data() != nullptr);
        CHECK(view.c_str()[0] == '\0');
        CHECK(view.begin() == view.end());
        CHECK(view.rbegin() == view.rend());
        CHECK(view == std::string_view{});
        CHECK(view == empty.view());
        CHECK(view.compare(std::string_view{}) == 0);
        CHECK(view.icompare(std::string_view{}) == 0);
        CHECK(view.starts_with(std::string_view{}));
        CHECK(view.ends_with(std::string_view{}));
        CHECK(view.istarts_with(std::string_view{}));
        CHECK(view.iends_with(std::string_view{}));
        CHECK(view.istarts_with(empty.view()));
        CHECK(view.iends_with(empty.view()));
        CHECK_FALSE(view.istarts_with("a"));
        CHECK_FALSE(view.iends_with("a"));
        CHECK_FALSE(view.istarts_with('\0'));
        CHECK_FALSE(view.iends_with('\0'));
        CHECK(view.contains(std::string_view{}));
        CHECK_FALSE(view.starts_with('a'));
        CHECK_FALSE(view.ends_with('a'));
        CHECK_FALSE(view.contains('\0'));
        CHECK(view.find(std::string_view{}) == 0);
        CHECK(view.rfind(std::string_view{}) == 0);
        CHECK(view.find('a') == php::StringView::npos);
    }
    CHECK_FALSE(php::StringView{});
    CHECK(empty.view());
}

TEST_CASE("Value views query exact scalar types and return explicit fallbacks without coercion", "[php-cxx][views]")
{
    php::Value uninitialized;
    auto       null = php::Value::null();
    for (auto const view : {php::ValueView{}, uninitialized.view(), null.view()}) {
        CHECK(view.get_bool_or(true));
        CHECK(view.get_long_or(42) == 42);
        CHECK(view.get_double_or(1.25) == 1.25);
        CHECK_FALSE(view.is_bool());
        CHECK_FALSE(view.is_long());
        CHECK_FALSE(view.is_double());
        CHECK(view.dereference().get() == view.get());
    }
    CHECK(php::ValueView{}.is_undef());
    CHECK(uninitialized.view().is_undef());
    CHECK_FALSE(null.view().is_undef());
    CHECK(null.view().is_null());

    zval slot{};
    for (bool const boolean : {false, true}) {
        ZVAL_BOOL(&slot, boolean);
        auto const view = php::ValueView(&slot);
        CHECK(view.is_bool());
        CHECK(view.is_true() == boolean);
        CHECK(view.is_false() == !boolean);
        CHECK(view.get_bool() == boolean);
        CHECK(view.get_bool_or(!boolean) == boolean);
        CHECK(view.get_long_or(42) == 42);
    }

    for (zend_long const number :
         {std::numeric_limits<zend_long>::min(), zend_long{0}, std::numeric_limits<zend_long>::max()}) {
        ZVAL_LONG(&slot, number);
        auto const view = php::ValueView(&slot);
        CHECK(view.is_long());
        CHECK(view.get_long() == number);
        CHECK(view.get_long_or(42) == number);
        CHECK_FALSE(view.is_bool());
        CHECK(view.get_bool_or(true));
        CHECK(view.get_double_or(1.25) == 1.25);
    }

    ZVAL_DOUBLE(&slot, 3.5);
    auto const real = php::ValueView(&slot);
    CHECK(real.is_double());
    CHECK(real.get_double() == 3.5);
    CHECK(real.get_double_or(1.25) == 3.5);
    CHECK_FALSE(real.is_long());
    CHECK(real.get_long_or(42) == 42);

    auto text = php::String::create("123");
    ZVAL_STR(&slot, text.release());
    auto string = php::Value::adopt(slot);
    CHECK(string.view().is_string());
    CHECK(string.view().get_string() == "123");
    CHECK(string.view().get_long_or(42) == 42);
    CHECK(string.view().get_double_or(1.25) == 1.25);
    CHECK_FALSE(string.view().is_array());
    CHECK_FALSE(string.view().is_object());
    CHECK_FALSE(string.view().is_resource());
    CHECK_FALSE(string.view().is_reference());
}

TEST_CASE("Value truthiness follows PHP scalar conversion without changing stored types", "[php-cxx][views]")
{
    php::Value undefined;
    auto       null = php::Value::null();
    CHECK_FALSE(php::ValueView{}.is_truthy());
    CHECK_FALSE(undefined.view().is_truthy());
    CHECK_FALSE(null.view().is_truthy());

    zval slot{};
    for (bool const boolean : {false, true}) {
        ZVAL_BOOL(&slot, boolean);
        CHECK(php::ValueView(&slot).is_truthy() == boolean);
        CHECK(php::ValueView(&slot).get_bool() == boolean);
    }
    ZVAL_LONG(&slot, 0);
    CHECK_FALSE(php::ValueView(&slot).is_truthy());
    for (zend_long const number : {zend_long{-1}, zend_long{1}, std::numeric_limits<zend_long>::min()}) {
        ZVAL_LONG(&slot, number);
        auto const view = php::ValueView(&slot);
        CHECK(view.is_truthy());
        CHECK_FALSE(view.is_true());
        CHECK(view.get_long() == number);
    }
    for (double const number : {0.0, -0.0}) {
        ZVAL_DOUBLE(&slot, number);
        CHECK_FALSE(php::ValueView(&slot).is_truthy());
    }
    for (double const number : {-0.5, 0.5, std::numeric_limits<double>::infinity()}) {
        ZVAL_DOUBLE(&slot, number);
        auto const view = php::ValueView(&slot);
        CHECK(view.is_truthy());
        CHECK(view.get_double() == number);
    }

    struct StringCase {
        std::string_view bytes;
        bool             truth;
    };

    StringCase const strings[]{
        {.bytes = "", .truth = false},
        {.bytes = "0", .truth = false},
        {.bytes = "false", .truth = true},
        {.bytes = "00", .truth = true},
        {.bytes = "0.0", .truth = true},
        {.bytes = std::string_view("\0", 1), .truth = true},
        {.bytes = std::string_view("0\0", 2), .truth = true},
    };
    for (auto const& test : strings) {
        auto text = php::String::create(test.bytes);
        ZVAL_STR(&slot, text.release());
        auto       owner = php::Value::adopt(slot);
        auto const count = GC_REFCOUNT(Z_STR_P(owner.get()));
        CHECK(owner.view().is_truthy() == test.truth);
        CHECK(owner.view().get_string() == test.bytes);
        CHECK(GC_REFCOUNT(Z_STR_P(owner.get())) == count);
    }
}

TEST_CASE("Value truthiness unwraps references and borrows array payloads", "[php-cxx][views]")
{
    for (bool const populated : {false, true}) {
        auto array = php::Array::create();
        if (populated) {
            // A nonempty array is true even when its only element is false-like.
            insert_integer(array.writable(), 0, 0);
        }
        zval slot{};
        ZVAL_ARR(&slot, array.release());
        auto       owner = php::Value::adopt(slot);
        auto const count = GC_REFCOUNT(Z_ARR_P(owner.get()));
        CHECK(owner.view().is_truthy() == populated);
        CHECK(owner.view().is_array());
        CHECK(GC_REFCOUNT(Z_ARR_P(owner.get())) == count);
    }

    for (zend_long const number : {zend_long{0}, zend_long{7}}) {
        zval slot{};
        ZVAL_LONG(&slot, number);
        ZVAL_MAKE_REF(&slot);
        auto       owner = php::Value::adopt(slot);
        auto const count = GC_REFCOUNT(Z_REF_P(owner.get()));
        CHECK(owner.view().is_truthy() == (number != 0));
        CHECK(owner.view().is_reference());
        CHECK_FALSE(owner.view().is_long());
        CHECK(owner.view().dereference().get_long() == number);
        CHECK(GC_REFCOUNT(Z_REF_P(owner.get())) == count);
    }
}

TEST_CASE("Value truthiness honors Zend object conversion handlers", "[php-cxx][views]")
{
    // Declare handlers before the owner so they remain alive through object destruction.
    auto handlers = *zend_get_std_object_handlers();
    zval slot{};
    object_init(&slot);
    auto       owner  = php::Value::adopt(slot);
    auto*      object = Z_OBJ_P(owner.get());
    auto const count  = GC_REFCOUNT(object);
    CHECK(owner.view().is_truthy());

    handlers.cast_object = [](zend_object*, zval* result, int type) noexcept -> zend_result {
        if (type != _IS_BOOL) {
            return FAILURE;
        }
        ZVAL_FALSE(result);
        return SUCCESS;
    };
    object->handlers = &handlers;
    CHECK_FALSE(owner.view().is_truthy());
    CHECK(owner.view().is_object());
    CHECK(owner.view().get_object() == object);
    CHECK(GC_REFCOUNT(object) == count);
}

TEST_CASE("Value payload getters borrow strings arrays objects and resources without retaining", "[php-cxx][views]")
{
    auto text = php::String::create(std::string_view("a\0b", 3));
    zval slot{};
    ZVAL_STR(&slot, text.release());
    auto       string       = php::Value::adopt(slot);
    auto const string_count = GC_REFCOUNT(Z_STR_P(string.get()));
    CHECK(string.view().get_string().size() == 3);
    CHECK(string.view().get_string()[2] == 'b');
    CHECK(GC_REFCOUNT(Z_STR_P(string.get())) == string_count);

    auto array = php::Array::create();
    insert_integer(array.writable(), 0, 7);
    ZVAL_ARR(&slot, array.release());
    auto       value       = php::Value::adopt(slot);
    auto const array_count = GC_REFCOUNT(Z_ARR_P(value.get()));
    CHECK(value.view().is_array());
    CHECK(value.view().get_array()[0].get_long() == 7);
    CHECK(GC_REFCOUNT(Z_ARR_P(value.get())) == array_count);

    object_init(&slot);
    auto       object       = php::Value::adopt(slot);
    auto const object_count = GC_REFCOUNT(Z_OBJ_P(object.get()));
    CHECK(object.view().is_object());
    CHECK(object.view().get_object() == Z_OBJ_P(object.get()));
    CHECK(GC_REFCOUNT(Z_OBJ_P(object.get())) == object_count);

    auto const resource_type = zend_register_list_destructors_ex(nullptr, nullptr, "phpcxx-view-test", 0);
    ZVAL_RES(&slot, zend_register_resource(nullptr, resource_type));
    auto       resource       = php::Value::adopt(slot);
    auto const resource_count = GC_REFCOUNT(Z_RES_P(resource.get()));
    CHECK(resource.view().is_resource());
    CHECK(resource.view().get_resource() == Z_RES_P(resource.get()));
    CHECK(resource.view().is_truthy());
    CHECK(GC_REFCOUNT(Z_RES_P(resource.get())) == resource_count);
}

TEST_CASE("Reference views require explicit dereferencing and preserve reference counts", "[php-cxx][views]")
{
    zval slot{};
    ZVAL_LONG(&slot, 19);
    ZVAL_MAKE_REF(&slot);
    auto       owner = php::Value::adopt(slot);
    auto const view  = owner.view();
    auto const count = GC_REFCOUNT(Z_REF_P(owner.get()));

    CHECK(view.is_reference());
    CHECK_FALSE(view.is_long());
    CHECK(view.get_long_or(42) == 42);
    auto const dereferenced = view.dereference();
    CHECK(dereferenced.is_long());
    CHECK(dereferenced.get_long() == 19);
    CHECK(dereferenced.dereference().get() == dereferenced.get());
    CHECK(GC_REFCOUNT(Z_REF_P(owner.get())) == count);
}

TEST_CASE("Array lookup follows PHP integer and binary string key rules without insertion", "[php-cxx][views]")
{
    auto  array = php::Array::create();
    auto* table = array.writable();
    insert_integer(table, 1, 10);
    insert_integer(table, -1, 20);
    insert_integer(table, "01", 30);
    insert_integer(table, "+1", 40);
    insert_integer(table, "", 50);
    insert_integer(table, std::string_view("a\0b", 3), 60);
    insert_integer(table, "9223372036854775808", 70);
    insert_integer(table, std::numeric_limits<zend_long>::min(), 80);
    insert_integer(table, std::numeric_limits<zend_long>::max(), 90);
    zval null{};
    ZVAL_NULL(&null);
    REQUIRE(zend_hash_str_update(table, "null", 4, &null) != nullptr);

    auto const view  = array.view();
    auto const count = GC_REFCOUNT(array.get());
    CHECK(view.find(1).get_long() == 10);
    CHECK(view["1"].get_long() == 10);
    CHECK(view[-1].get_long() == 20);
    CHECK(view["-1"].get_long() == 20);
    CHECK(view["01"].get_long() == 30);
    CHECK(view["+1"].get_long() == 40);
    CHECK(view[std::string_view{}].get_long() == 50);
    CHECK(view[std::string_view("a\0b", 3)].get_long() == 60);
    CHECK(view["9223372036854775808"].get_long() == 70);
    CHECK(view["-9223372036854775808"].get_long() == 80);
    CHECK(view["9223372036854775807"].get_long() == 90);
    CHECK(view.contains(1));
    CHECK(view.contains("1"));
    CHECK(view.contains("null"));
    CHECK(view["null"].is_null());
    CHECK_FALSE(view.contains(0));
    CHECK_FALSE(view.contains("absent"));
    CHECK(view.find(0).get() == nullptr);
    CHECK(view["absent"].get() == nullptr);
    CHECK(view["absent"].is_undef());
    CHECK(view.size() == 10);
    CHECK_FALSE(view.empty());
    CHECK(GC_REFCOUNT(array.get()) == count);
}

TEST_CASE("Array iterators traverse sparse packed arrays without changing PHP's array pointer", "[php-cxx][views]")
{
    auto  array = php::Array::create();
    auto* table = array.writable();
    for (zend_long index = 0; index != 5; ++index) {
        insert_integer(table, index, index * 10);
    }
    REQUIRE(HT_IS_PACKED(table));
    REQUIRE(zend_hash_index_del(table, 0) == SUCCESS);
    REQUIRE(zend_hash_index_del(table, 2) == SUCCESS);
    REQUIRE(zend_hash_index_del(table, 4) == SUCCESS);
    zend_hash_internal_pointer_end(table);
    auto const position = zend_hash_get_current_pos(table);
    auto const count    = GC_REFCOUNT(table);
    auto const view     = array.view();

    auto first = view.begin();
    auto copy  = first;
    CHECK(first == copy);
    CHECK(std::holds_alternative<zend_long>(first->key));
    CHECK(first->value.get_long() == 10);
    auto previous = first++;
    CHECK(previous == copy);
    CHECK(first != copy);
    CHECK(first->value.get_long() == 30);
    CHECK(copy->value.get_long() == 10);
    CHECK(++first == view.end());
    CHECK(std::distance(view.cbegin(), view.cend()) == 2);

    std::vector<zend_long> keys;
    std::vector<zend_long> values;
    for (auto const [key, value] : view) {
        keys.push_back(std::get<zend_long>(key));
        values.push_back(value.get_long());
    }
    CHECK(keys == std::vector<zend_long>{1, 3});
    CHECK(values == std::vector<zend_long>{10, 30});
    CHECK(zend_hash_get_current_pos(table) == position);
    CHECK(GC_REFCOUNT(table) == count);
}

TEST_CASE("Array iteration preserves mixed key order and reference wrappers", "[php-cxx][views]")
{
    auto  array = php::Array::create();
    auto* table = array.writable();
    insert_integer(table, "first", 10);
    insert_integer(table, -7, 20);
    insert_integer(table, std::string_view("a\0b", 3), 30);
    insert_integer(table, "deleted", 40);
    insert_integer(table, 100, 50);
    REQUIRE(zend_hash_str_del(table, "deleted", 7) == SUCCESS);
    zval reference{};
    ZVAL_LONG(&reference, 60);
    ZVAL_MAKE_REF(&reference);
    REQUIRE(zend_hash_str_update(table, "ref", 3, &reference) != nullptr);
    ZVAL_UNDEF(&reference);
    auto const view = array.view();

    auto cursor = view.begin();
    CHECK(std::get<php::StringView>(cursor->key) == "first");
    CHECK((cursor++)->value.get_long() == 10);
    CHECK(std::get<zend_long>(cursor->key) == -7);
    ++cursor;
    CHECK(std::get<php::StringView>(cursor->key) == std::string_view("a\0b", 3));
    ++cursor;
    CHECK(std::get<zend_long>(cursor->key) == 100);
    ++cursor;
    CHECK(cursor->value.is_reference());
    CHECK(cursor->value.dereference().get_long() == 60);
    CHECK(++cursor == view.end());
    CHECK(view["ref"].is_reference());
    CHECK(view["ref"].dereference().get_long() == 60);

    // Exercise a standard input-iterator algorithm without requiring C++20 ranges.
    auto const total = std::accumulate(view.begin(), view.end(), zend_long{0}, [](zend_long sum, auto const& entry) {
        return sum + entry.value.get_long_or(0);
    });
    CHECK(total == 110);
}

TEST_CASE("Absent empty immutable and fully deleted arrays have empty iterator ranges", "[php-cxx][views]")
{
    auto empty   = php::Array::create();
    auto deleted = php::Array::create();
    insert_integer(deleted.writable(), 0, 1);
    REQUIRE(zend_hash_index_del(deleted.writable(), 0) == SUCCESS);
    zval immutable{};
    ZVAL_EMPTY_ARRAY(&immutable);

    for (auto const view : {php::ArrayView{}, empty.view(), deleted.view(), php::ArrayView(Z_ARR(immutable))}) {
        CHECK(view.empty());
        CHECK(view.size() == 0);
        CHECK(view.begin() == view.end());
        CHECK(view.cbegin() == view.cend());
        CHECK(std::distance(view.begin(), view.end()) == 0);
        CHECK_FALSE(view.contains(0));
        CHECK_FALSE(view.contains("missing"));
        CHECK(view.find(0).get() == nullptr);
        CHECK(view.find(std::string_view{}).get() == nullptr);
    }
    CHECK(php::ArrayView::Iterator{} == php::ArrayView::Iterator{});
}
