#include <phpcxx/array.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/views.hpp>
#include <phpcxx/zend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace {

static_assert(!std::is_copy_constructible_v<php::String> && !std::is_copy_assignable_v<php::String>);
static_assert(!std::is_copy_constructible_v<php::Array> && !std::is_copy_assignable_v<php::Array>);
static_assert(!std::is_copy_constructible_v<php::Value> && !std::is_copy_assignable_v<php::Value>);
static_assert(std::is_nothrow_move_constructible_v<php::String> && std::is_nothrow_move_assignable_v<php::String>);
static_assert(std::is_nothrow_move_constructible_v<php::Array> && std::is_nothrow_move_assignable_v<php::Array>);
static_assert(std::is_nothrow_move_constructible_v<php::Value> && std::is_nothrow_move_assignable_v<php::Value>);
static_assert(std::is_trivially_copyable_v<php::StringView> && std::is_trivially_copyable_v<php::ArrayView> &&
              std::is_trivially_copyable_v<php::ValueView>);

auto set_integer(php::Array& array, zend_ulong index, zend_long number) -> void
{
    zval value{};
    ZVAL_LONG(&value, number);
    REQUIRE(zend_hash_index_update(array.writable(), index, &value) != nullptr);
}

} // namespace

TEST_CASE("Absent owners and views are safe and distinct from valid empty PHP values", "[php-cxx]")
{
    php::String string;
    CHECK_FALSE(string);
    CHECK_FALSE(string.view());
    CHECK(string.view().bytes().empty());
    CHECK_FALSE(string.duplicate());
    CHECK_FALSE(php::String::adopt(nullptr));
    CHECK_FALSE(php::String::retain(php::StringView{}));
    CHECK(string.release() == nullptr);

    auto empty_string = php::String::create({});
    CHECK(empty_string);
    CHECK(empty_string.view().bytes().empty());

    php::Array array;
    CHECK_FALSE(array);
    CHECK_FALSE(array.view());
    CHECK(array.view().size() == 0);
    CHECK(array.writable() == nullptr);
    CHECK_FALSE(array.duplicate());
    CHECK_FALSE(php::Array::adopt(nullptr));
    CHECK_FALSE(php::Array::retain(php::ArrayView{}));
    CHECK(array.release() == nullptr);

    auto empty_array = php::Array::create();
    CHECK(empty_array);
    CHECK(empty_array.view().size() == 0);

    php::Value value;
    CHECK_FALSE(value.is_initialized());
    CHECK_FALSE(value.is_null());
    CHECK_FALSE(value.duplicate().is_initialized());
    CHECK_FALSE(php::ValueView{}.is_initialized());
    CHECK_FALSE(php::Value::retain(php::ValueView{}).is_initialized());
    auto released = value.release();
    CHECK(Z_TYPE(released) == IS_UNDEF);
    auto adopted = php::Value::adopt(released);
    CHECK_FALSE(adopted.is_initialized());
    CHECK(Z_TYPE(released) == IS_UNDEF);

    auto null = php::Value::null();
    CHECK(null.is_initialized());
    CHECK(null.is_null());
    auto moved_null = std::move(null);
    CHECK(moved_null.is_null());
    CHECK_FALSE(null.is_initialized());
}

TEST_CASE("String owners transfer references and preserve binary bytes", "[php-cxx]")
{
    constexpr std::string_view bytes("before\0after", 12);
    auto*                      raw   = zend_string_init(bytes.data(), bytes.size(), false);
    auto                       owner = php::String::adopt(raw);
    REQUIRE(owner.get() == raw);
    CHECK(GC_REFCOUNT(raw) == 1);
    CHECK(owner.view().bytes() == bytes);

    {
        auto const borrowed    = owner.view();
        auto const copied_view = borrowed;
        CHECK(copied_view.get() == raw);
        CHECK(GC_REFCOUNT(raw) == 1);

        auto retained = php::String::retain(borrowed);
        CHECK(GC_REFCOUNT(raw) == 2);
        auto moved = std::move(retained);
        CHECK_FALSE(retained);
        CHECK(GC_REFCOUNT(raw) == 2);

        auto& alias = moved;
        moved       = std::move(alias);
        CHECK(moved.get() == raw);
        CHECK(GC_REFCOUNT(raw) == 2);
    }
    CHECK(GC_REFCOUNT(raw) == 1);

    auto duplicated = owner.duplicate();
    CHECK(duplicated.get() != raw);
    CHECK(duplicated.view().bytes() == bytes);
    auto old_reference = php::String::retain(duplicated.view());
    duplicated         = std::move(owner);
    CHECK_FALSE(owner);
    CHECK(GC_REFCOUNT(old_reference.get()) == 1);

    auto* released = duplicated.release();
    CHECK_FALSE(duplicated);
    CHECK(released == raw);
    CHECK(GC_REFCOUNT(released) == 1);
    zend_string_release(released);
}

TEST_CASE("Interned strings obey Zend retention and duplication rules", "[php-cxx]")
{
    // Request-time interning is optional in newer PHP. Use Zend's already-interned empty string.
    auto* raw = ZSTR_EMPTY_ALLOC();
    REQUIRE(ZSTR_IS_INTERNED(raw));
    auto const count      = GC_REFCOUNT(raw);
    auto       owner      = php::String::adopt(raw);
    auto       retained   = php::String::retain(owner.view());
    auto       duplicated = owner.duplicate();
    CHECK(retained.get() == raw);
    CHECK(duplicated.get() == raw);
    CHECK(GC_REFCOUNT(raw) == count);
    CHECK(duplicated.view().bytes().empty());
}

TEST_CASE("Array writes separate shared storage and released references survive owners", "[php-cxx]")
{
    auto* raw   = zend_new_array(0);
    auto  owner = php::Array::adopt(raw);
    set_integer(owner, 0, 7);
    auto const borrowed    = owner.view();
    auto const copied_view = borrowed;
    CHECK(copied_view.size() == 1);
    CHECK(GC_REFCOUNT(raw) == 1);

    auto retained = php::Array::retain(borrowed);
    CHECK(GC_REFCOUNT(raw) == 2);
    auto moved = std::move(retained);
    CHECK_FALSE(retained);
    CHECK(GC_REFCOUNT(raw) == 2);

    auto& alias = moved;
    moved       = std::move(alias);
    CHECK(moved.get() == raw);
    CHECK(GC_REFCOUNT(raw) == 2);

    set_integer(moved, 0, 9);
    CHECK(moved.get() != raw);
    REQUIRE(zend_hash_index_find(raw, 0) != nullptr);
    CHECK(Z_LVAL_P(zend_hash_index_find(raw, 0)) == 7);
    CHECK(GC_REFCOUNT(raw) == 1);

    auto duplicated = owner.duplicate();
    CHECK(duplicated.get() != raw);
    CHECK(duplicated.view().size() == 1);
    auto old_reference = php::Array::retain(moved.view());
    moved              = std::move(owner);
    CHECK_FALSE(owner);
    CHECK(GC_REFCOUNT(old_reference.get()) == 1);

    auto* released = moved.release();
    CHECK_FALSE(moved);
    CHECK(released == raw);
    CHECK(Z_LVAL_P(zend_hash_index_find(released, 0)) == 7);
    zend_array_release(released);
}

TEST_CASE("Immutable empty arrays are retained without refcount changes and separated before writes", "[php-cxx]")
{
    // This engine-owned singleton is deliberately immutable; it must never be freed or changed.
    zval empty{};
    ZVAL_EMPTY_ARRAY(&empty);
    auto* raw = Z_ARR(empty);
    REQUIRE((GC_FLAGS(raw) & IS_ARRAY_IMMUTABLE) != 0);
    auto const count = GC_REFCOUNT(raw);

    auto owner    = php::Array::adopt(raw);
    auto retained = php::Array::retain(owner.view());
    CHECK(GC_REFCOUNT(raw) == count);
    CHECK(retained.get() == raw);
    auto duplicated = owner.duplicate();
    CHECK(duplicated.get() != raw);

    set_integer(owner, 0, 11);
    CHECK(owner.get() != raw);
    CHECK(retained.view().size() == 0);
    CHECK(GC_REFCOUNT(raw) == count);
}

TEST_CASE("Value retention matches ZVAL_COPY and adoption consumes the source slot", "[php-cxx]")
{
    auto  text = php::String::create("owned");
    auto* raw  = text.release();
    zval  source{};
    ZVAL_STR(&source, raw);
    auto owner = php::Value::adopt(source);
    CHECK(Z_TYPE(source) == IS_UNDEF);
    CHECK(GC_REFCOUNT(raw) == 1);
    auto const borrowed = owner.view();
    CHECK(borrowed.type() == IS_STRING);
    CHECK(GC_REFCOUNT(raw) == 1);

    {
        auto retained = php::Value::retain(borrowed);
        CHECK(Z_STR_P(retained.get()) == raw);
        CHECK(GC_REFCOUNT(raw) == 2);
        auto moved = std::move(retained);
        CHECK_FALSE(retained.is_initialized());
        CHECK(GC_REFCOUNT(raw) == 2);

        auto& alias = moved;
        moved       = std::move(alias);
        CHECK(moved.view().type() == IS_STRING);
        CHECK(GC_REFCOUNT(raw) == 2);
    }
    CHECK(GC_REFCOUNT(raw) == 1);

    auto old_text = php::String::create("replaced");
    ZVAL_STR(&source, zend_string_copy(const_cast<zend_string*>(old_text.get())));
    auto replacement = php::Value::adopt(source);
    replacement      = std::move(owner);
    CHECK_FALSE(owner.is_initialized());
    CHECK(GC_REFCOUNT(old_text.get()) == 1);
    auto duplicate = replacement.duplicate();
    CHECK(Z_STR_P(duplicate.get()) == raw);
    CHECK(GC_REFCOUNT(raw) == 2);

    auto released = replacement.release();
    CHECK_FALSE(replacement.is_initialized());
    CHECK(Z_STR(released) == raw);
    zval_ptr_dtor(&released);
    CHECK(GC_REFCOUNT(raw) == 1);
}

TEST_CASE("Value duplication separates direct arrays while preserving nested PHP identity", "[php-cxx]")
{
    zval nested{};
    array_init(&nested);
    zval object{};
    object_init(&object);
    zval reference{};
    ZVAL_LONG(&reference, 5);
    ZVAL_MAKE_REF(&reference);
    auto  reference_owner  = php::Value::adopt(reference);
    auto  reference_entry  = php::Value::retain(reference_owner.view()).release();
    auto* shared_reference = Z_REF_P(reference_owner.get());

    auto array = php::Array::create();
    REQUIRE(zend_hash_index_update(array.writable(), 0, &nested) != nullptr);
    REQUIRE(zend_hash_index_update(array.writable(), 1, &object) != nullptr);
    REQUIRE(zend_hash_index_update(array.writable(), 2, &reference_entry) != nullptr);
    // The hash now owns these values. The raw slots only borrow their payloads below.
    zval source{};
    ZVAL_ARR(&source, array.release());
    auto owner    = php::Value::adopt(source);
    auto retained = php::Value::retain(owner.view());
    CHECK(Z_ARR_P(retained.get()) == Z_ARR_P(owner.get()));

    auto  duplicated      = owner.duplicate();
    auto* duplicate_array = Z_ARR_P(duplicated.get());
    CHECK(duplicate_array != Z_ARR_P(owner.get()));
    CHECK(Z_ARR_P(zend_hash_index_find(duplicate_array, 0)) == Z_ARR(nested));
    CHECK(Z_OBJ_P(zend_hash_index_find(duplicate_array, 1)) == Z_OBJ(object));
    CHECK(Z_REF_P(zend_hash_index_find(duplicate_array, 2)) == shared_reference);

    ZVAL_LONG(&shared_reference->val, 19);
    CHECK(Z_LVAL_P(Z_REFVAL_P(zend_hash_index_find(duplicate_array, 2))) == 19);
}

TEST_CASE("Value copies preserve a PHP reference wrapper rather than dereferencing it", "[php-cxx]")
{
    zval source{};
    ZVAL_LONG(&source, 23);
    ZVAL_MAKE_REF(&source);
    auto  owner     = php::Value::adopt(source);
    auto* reference = Z_REF_P(owner.get());
    CHECK(GC_REFCOUNT(reference) == 1);
    {
        auto retained   = php::Value::retain(owner.view());
        auto duplicated = owner.duplicate();
        CHECK(retained.view().type() == IS_REFERENCE);
        CHECK(Z_REF_P(retained.get()) == reference);
        CHECK(Z_REF_P(duplicated.get()) == reference);
        CHECK(GC_REFCOUNT(reference) == 3);

        ZVAL_LONG(&reference->val, 29);
        CHECK(Z_LVAL_P(Z_REFVAL_P(retained.get())) == 29);
        CHECK(Z_LVAL_P(Z_REFVAL_P(duplicated.get())) == 29);
    }
    CHECK(GC_REFCOUNT(reference) == 1);
}

TEST_CASE("Array and Value destruction release their contained references", "[php-cxx]")
{
    auto  text = php::String::create("contained");
    auto* raw  = const_cast<zend_string*>(text.get());
    {
        auto array = php::Array::create();
        zval entry{};
        ZVAL_STR(&entry, zend_string_copy(raw));
        REQUIRE(zend_hash_index_update(array.writable(), 0, &entry) != nullptr);
        CHECK(GC_REFCOUNT(raw) == 2);

        zval source{};
        ZVAL_ARR(&source, array.release());
        auto value    = php::Value::adopt(source);
        auto retained = php::Value::retain(value.view());
        CHECK(GC_REFCOUNT(raw) == 2);
    }
    CHECK(GC_REFCOUNT(raw) == 1);
}

TEST_CASE("Dropping an array owner makes its unreachable cycle collectible", "[php-cxx]")
{
    auto text = php::String::create("cycle payload");
    {
        auto* raw = zend_new_array(2);
        zval  payload{};
        auto  retained_text = php::String::retain(text.view());
        ZVAL_STR(&payload, retained_text.release());
        REQUIRE(zend_hash_index_update(raw, 1, &payload) != nullptr);

        // Finish the table while it is unique, then account for the installed
        // self-edge. Zend's debug checks forbid hash writes to a shared array.
        zval self{};
        ZVAL_ARR(&self, raw);
        REQUIRE(zend_hash_index_update(raw, 0, &self) != nullptr);
        GC_ADDREF(raw);
        auto owner = php::Array::adopt(raw);
        CHECK(GC_REFCOUNT(text.get()) == 2);
    }

    // Only the array's self-reference remains. Owner destruction must nominate
    // it as a possible GC root so the retained string can be released now.
    gc_collect_cycles();
    CHECK(GC_REFCOUNT(text.get()) == 1);
}
