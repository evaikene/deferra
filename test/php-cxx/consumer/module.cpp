#include <phpcxx/array.hpp>
#include <phpcxx/build.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/views.hpp>
#include <phpcxx/zend.hpp>

#include <string_view>

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
    return string_view.bytes() == std::string_view("a\0b", 3) && array_view.size() == 1 && separated != array.get() &&
                   value_view.type() == IS_ARRAY && null.is_null() && duplicate_value.is_initialized()
             ? SUCCESS
             : FAILURE;
}

zend_module_entry module{.size                 = sizeof(zend_module_entry),
                         .zend_api             = ZEND_MODULE_API_NO,
                         .zend_debug           = ZEND_DEBUG,
                         .zts                  = USING_ZTS,
                         .name                 = "phpcxx_consumer",
                         .module_startup_func  = start_module,
                         .request_startup_func = start_request,
                         .version              = "0.1.0",
                         .build_id             = ZEND_MODULE_BUILD_ID};

} // namespace

extern "C" ZEND_DLEXPORT auto get_module() -> zend_module_entry*
{
    return &module;
}
