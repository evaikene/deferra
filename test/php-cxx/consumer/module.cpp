#include "functions.hpp"

#include <phpcxx/array.hpp>
#include <phpcxx/build.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/views.hpp>
#include <phpcxx/zend.hpp>

#include <string_view>
#include <variant>

namespace {

auto start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) -> zend_result
{
    auto const& info = php::build_info();
    if (info.module_api != ZEND_MODULE_API_NO || info.debug != (ZEND_DEBUG != 0) || info.zts != (USING_ZTS != 0)) {
        return FAILURE;
    }
    return SUCCESS;
}

auto start_request([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // Exercise the independently linked ownership API only inside a PHP request.
    auto string             = php::String::create(std::string_view("a\0b", 3));
    auto retained_string    = php::String::retain(string.view());
    auto duplicate_string   = string.duplicate();
    auto transferred_string = php::String::adopt(retained_string.release());

    auto array = php::Array::create();
    zval entry{};
    ZVAL_STR(&entry, transferred_string.release());
    zend_hash_index_update(array.writable(), 0, &entry);
    auto        retained_array    = php::Array::retain(array.view());
    auto        duplicate_array   = array.duplicate();
    auto        transferred_array = php::Array::adopt(retained_array.release());
    auto const* separated         = transferred_array.writable();

    zval slot{};
    ZVAL_ARR(&slot, duplicate_array.release());
    auto value             = php::Value::adopt(slot);
    auto retained_value    = php::Value::retain(value.view());
    auto duplicate_value   = value.duplicate();
    auto released          = retained_value.release();
    auto transferred_value = php::Value::adopt(released);
    auto null              = php::Value::null();

    php::StringView const string_view = duplicate_string.view();
    php::ArrayView const  array_view  = array.view();
    php::ValueView const  value_view  = transferred_value.view();
    auto const            strings_match =
        string_view == std::string_view("a\0b", 3) && string_view.c_str()[string_view.size()] == '\0' &&
        string_view[2] == 'b' && string_view.starts_with('a') && string_view.ends_with('b') &&
        string_view.contains('\0') && string_view.find('b') == 2 && string_view.rfind('a') == 0 &&
        string_view.icompare(std::string_view("A\0B", 3)) == 0 && string_view.iequals(std::string_view("A\0B", 3)) &&
        string_view.istarts_with('A') && string_view.iends_with('B') &&
        string_view.istarts_with(std::string_view("A\0", 2)) && string_view.iends_with(std::string_view("\0B", 2)) &&
        string_view.istarts_with(string_view) && string_view.iends_with(string_view);

    auto const first = array_view.begin();
    if (first == array_view.end()) {
        return FAILURE;
    }
    auto const first_entry = *first;
    auto const arrays_match =
        array_view.size() == 1 && array_view.contains("0") && array_view[0].get_string() == string_view &&
        std::holds_alternative<zend_long>(first_entry.key) && std::get<zend_long>(first_entry.key) == 0 &&
        first_entry.value.get_string() == string_view && separated != array.get();

    auto const values_match = value_view.is_array() && value_view.get_array().size() == 1 && value_view.is_truthy() &&
                              value_view.get_long_or(42) == 42 && null.is_null() && !null.view().is_truthy() &&
                              !php::ValueView{}.is_truthy() && duplicate_value.is_initialized();
    return strings_match && arrays_match && values_match ? SUCCESS : FAILURE;
}

zend_module_entry module{.size                 = sizeof(zend_module_entry),
                         .zend_api             = ZEND_MODULE_API_NO,
                         .zend_debug           = ZEND_DEBUG,
                         .zts                  = USING_ZTS,
                         .name                 = "phpcxx_consumer",
                         .functions            = phpcxx_test_functions(),
                         .module_startup_func  = start_module,
                         .request_startup_func = start_request,
                         .version              = "0.1.0",
                         .build_id             = ZEND_MODULE_BUILD_ID};

} // namespace

extern "C" ZEND_DLEXPORT auto get_module() -> zend_module_entry*
{
    return &module;
}
