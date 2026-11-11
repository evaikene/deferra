#include "consumer/objects.hpp"

#include <phpcxx/exception.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

auto evaluate(std::string_view expression) -> php::Value
{
    zval       result{};
    auto const status    = zend_eval_stringl(expression.data(), expression.size(), &result, "php-cxx binding test");
    auto       owner     = php::Value::adopt(result);
    // Clear unexpected errors before a failed assertion unwinds the test scope.
    bool const exception = php::has_exception();
    if (exception) {
        zend_clear_exception();
    }
    REQUIRE(status == SUCCESS);
    REQUIRE_FALSE(exception);
    return owner;
}

auto allocate(std::string_view class_name) -> php::Value
{
    auto  name  = php::String::create(class_name);
    auto* entry = zend_lookup_class(const_cast<zend_string*>(name.get()));
    REQUIRE(entry);
    zval       slot{};
    auto const result = object_init_ex(&slot, entry);
    auto       owner  = php::Value::adopt(slot);
    REQUIRE(result == SUCCESS);
    return owner;
}

} // namespace

TEST_CASE("Native binding separates Zend allocation from aligned construction", "[php-cxx][object]")
{
    static_assert(!std::is_standard_layout_v<phpcxx_test::Native>);
    static_assert(alignof(phpcxx_test::Native) == ZEND_MM_ALIGNMENT);
    auto const before = phpcxx_test::counts();
    {
        auto  owner  = allocate("PhpCxxNode");
        auto* object = Z_OBJ_P(owner.get());
        CHECK_FALSE(phpcxx_test::Binding::get(object));
        CHECK_FALSE(phpcxx_test::Binding::get(nullptr));
        CHECK_FALSE(phpcxx_test::Binding::construct(nullptr, 0, php::Value::null()));

        auto native = phpcxx_test::Binding::construct(object, 42, php::Value::null());
        REQUIRE(native);
        CHECK(reinterpret_cast<std::uintptr_t>(native.value()) % alignof(phpcxx_test::Native) == 0);
        auto const allocation = reinterpret_cast<std::uintptr_t>(object) - object->handlers->offset;
        CHECK(reinterpret_cast<std::uintptr_t>(native.value()) >= allocation);
        CHECK(reinterpret_cast<std::uintptr_t>(native.value()) + sizeof(phpcxx_test::Native) <=
              reinterpret_cast<std::uintptr_t>(object));
        CHECK(native.value()->number() == 42);
        CHECK(phpcxx_test::Binding::get(object, object->ce));
        CHECK_FALSE(phpcxx_test::Binding::get(object, zend_standard_class_def));
        CHECK_FALSE(phpcxx_test::LockedBinding::get(object));
        CHECK_FALSE(phpcxx_test::Binding::construct(object, 99, php::Value::null()));
        CHECK(native.value()->number() == 42);
    }
    auto const after = phpcxx_test::counts();
    CHECK(after.constructed == before.constructed + 1);
    CHECK(after.destroyed == before.destroyed + 1);
    CHECK(after.live == before.live);

    // A propertyless, never-constructed object must not run T's destructor.
    {
        auto empty = allocate("PhpCxxLocked");
    }
    CHECK(phpcxx_test::counts().destroyed == after.destroyed);

    auto  immobile = allocate("PhpCxxImmobile");
    auto* object   = Z_OBJ_P(immobile.get());
    CHECK(phpcxx_test::ImmobileBinding::construct(object));
    CHECK(phpcxx_test::ImmobileBinding::get(object));
    CHECK(object->handlers->clone_obj == nullptr);
}

TEST_CASE("Native GC reports reference wrappers without retaining them", "[php-cxx][object]")
{
    auto  owner  = allocate("PhpCxxNode");
    auto* object = Z_OBJ_P(owner.get());
    zval  slot{};
    ZVAL_COPY(&slot, owner.get());
    ZVAL_MAKE_REF(&slot);
    auto  reference     = php::Value::adopt(slot);
    auto* raw_reference = Z_REF_P(reference.get());
    auto  native        = phpcxx_test::Binding::construct(object, 0, std::move(reference));
    REQUIRE(native);
    auto const reference_count = GC_REFCOUNT(raw_reference);

    zval* table      = nullptr;
    int   count      = 0;
    auto* properties = object->handlers->get_gc(object, &table, &count);
    CHECK(properties == nullptr);
    REQUIRE(count == 1);
    CHECK(Z_ISREF(table[0]));
    CHECK(Z_REF(table[0]) == raw_reference);
    CHECK(GC_REFCOUNT(raw_reference) == reference_count);
    auto const live = phpcxx_test::counts().live;
    owner           = php::Value{};
    zend_gc_collect_cycles();
    CHECK(phpcxx_test::counts().live == live - 1);
}

TEST_CASE("Binding rejects incompatible class setup without mutation", "[php-cxx][object]")
{
    CHECK_FALSE(phpcxx_test::Binding::bind(nullptr));
    zend_class_entry entry{};
    entry.type = ZEND_USER_CLASS;
    CHECK_FALSE(phpcxx_test::Binding::bind(&entry));
    entry.type     = ZEND_INTERNAL_CLASS;
    entry.ce_flags = ZEND_ACC_INTERFACE;
    CHECK_FALSE(phpcxx_test::Binding::bind(&entry));
    entry.ce_flags      = 0;
    entry.create_object = zend_objects_new;
    CHECK_FALSE(phpcxx_test::Binding::bind(&entry));
    CHECK(entry.ce_flags == 0);
    CHECK(entry.create_object == zend_objects_new);
}

TEST_CASE("Bound objects obey PHP construction, cloning, ownership and GC contracts", "[php-cxx][object]")
{
    auto result = evaluate(std::string("include '") + PHPCXX_OBJECT_CHECK_PATH + "'");
    CHECK(result.view().is_true());
    CHECK(phpcxx_test::counts().live == 3);
}

TEST_CASE("Native comparison guards binding identity and construction state", "[php-cxx][object]")
{
    auto  left         = allocate("PhpCxxNode");
    auto  right        = allocate("PhpCxxNode");
    auto  foreign      = allocate("PhpCxxLocked");
    auto  compare      = Z_OBJ_P(left.get())->handlers->compare;
    auto* left_slot    = const_cast<zval*>(left.get());
    auto* right_slot   = const_cast<zval*>(right.get());
    auto* foreign_slot = const_cast<zval*>(foreign.get());

    // Neither operand is constructed yet. In particular, a same-class pair
    // must not reach the native policy just because its Zend properties match.
    CHECK(compare(left_slot, left_slot) == 0);
    CHECK(compare(left_slot, right_slot) == ZEND_UNCOMPARABLE);
    CHECK(compare(right_slot, left_slot) == ZEND_UNCOMPARABLE);
    REQUIRE(phpcxx_test::Binding::construct(Z_OBJ_P(left.get()), 10, php::Value::null()));
    CHECK(compare(left_slot, right_slot) == ZEND_UNCOMPARABLE);
    CHECK(compare(right_slot, left_slot) == ZEND_UNCOMPARABLE);
    CHECK(compare(left_slot, foreign_slot) == ZEND_UNCOMPARABLE);
    CHECK(compare(foreign_slot, left_slot) == ZEND_UNCOMPARABLE);
    REQUIRE(phpcxx_test::Binding::construct(Z_OBJ_P(right.get()), 20, php::Value::null()));
    CHECK(compare(left_slot, right_slot) == -1);
    CHECK(compare(right_slot, left_slot) == 1);

    // An unrelated default-policy binding keeps its standard handler unchanged.
    CHECK(Z_OBJ_P(foreign.get())->handlers->compare == zend_std_compare_objects);

    // As in the lifecycle fixture, install an exception without raising through
    // a nonexistent PHP call frame. Comparison must not execute PHP or replace it.
    zval exception{};
    object_init_ex(&exception, zend_ce_exception);
    EG(exception)  = Z_OBJ(exception);
    auto* original = EG(exception);
    CHECK(compare(left_slot, right_slot) == ZEND_UNCOMPARABLE);
    CHECK(EG(exception) == original);
    zend_clear_exception();
}
