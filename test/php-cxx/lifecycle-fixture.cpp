#include "lifecycle-fixture.hpp"

#include "consumer/functions.hpp"
#include "consumer/objects.hpp"

#include <phpcxx/array.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>

#include <Zend/zend_exceptions.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace phpcxx_test {
namespace {

auto start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    return register_classes();
}

auto start_sapi(sapi_module_struct* sapi) noexcept -> int
{
    // Zend mutates module bookkeeping during startup. Rebuild it for each runtime
    // rather than carrying the previous runtime's registration state forward.
    // Zend borrows the entry itself until MSHUTDOWN; it needs static storage.
    static zend_module_entry module;
    module = zend_module_entry{.size                = sizeof(zend_module_entry),
                               .zend_api            = ZEND_MODULE_API_NO,
                               .zend_debug          = ZEND_DEBUG,
                               .zts                 = USING_ZTS,
                               .name                = "phpcxx_lifecycle_fixture",
                               .functions           = phpcxx_test_functions(),
                               .module_startup_func = start_module,
                               .version             = "0.1.0",
                               .build_id            = ZEND_MODULE_BUILD_ID};
    return php_module_startup(sapi, &module);
}

auto exercise_owners() -> bool
{
    auto text  = php::String::create(std::string_view("a\0b", 3));
    auto array = php::Array::create();
    zval slot{};
    ZVAL_STR(&slot, text.release());
    zend_hash_next_index_insert(array.writable(), &slot);
    auto  shared   = php::Array::retain(array.view());
    auto* writable = shared.writable();
    ZVAL_LONG(&slot, 42);
    zend_hash_index_update(writable, 0, &slot);
    return array.view()[0].get_string() == std::string_view("a\0b", 3) && shared.view()[0].get_long() == 42;
}

} // namespace

Runtime::Runtime()
    : _saved_module(php_embed_module)
{
    // Embed borrows argv until request shutdown; static bytes outlive all cycles.
    static char  program[]          = "php-cxx-lifecycle-test";
    static char* arguments[]        = {program, nullptr};
    php_embed_module.php_ini_ignore = 1;
    php_embed_module.startup        = start_sapi;
    if (php_embed_init(1, arguments) == FAILURE) {
        // A partly initialized runtime cannot safely run another test or teardown.
        std::fputs("PHP Embed initialization failed\n", stderr);
        std::exit(EXIT_FAILURE);
    }
}

Runtime::~Runtime()
{
    php_embed_shutdown();
    php_embed_module = _saved_module;
}

auto Runtime::restart_request() -> bool
{
    php_request_shutdown(nullptr);
    bool const balanced = cleanup_balanced();
    // Keep a live request for Embed's eventual shutdown even if accounting fails.
    if (php_request_startup() == FAILURE) {
        std::fputs("PHP request restart failed\n", stderr);
        std::exit(EXIT_FAILURE);
    }
    SG(headers_sent)            = 1;
    SG(request_info).no_headers = true;
    return balanced;
}

auto evaluate_boolean(std::string_view expression) -> bool
{
    zval result{};
    ZVAL_UNDEF(&result);
    auto const status    = zend_eval_stringl(expression.data(), expression.size(), &result, "php-cxx lifecycle test");
    auto       owner     = php::Value::adopt(result);
    bool const exception = php::has_exception();
    if (exception) {
        zend_clear_exception();
    }
    return status == SUCCESS && !exception && owner.view().is_true();
}

auto exercise_request() -> bool
{
    // Re-declaring the shared scripts' functions/classes in each request also
    // detects leaked user metadata. Only their explicit PHP globals survive to
    // this request's shutdown, where native counters must return to zero.
    if (php::has_exception() || !cleanup_balanced() || !exercise_owners()) {
        return false;
    }
    if (!evaluate_boolean("!isset($GLOBALS['phpcxx_request_marker']) && !isset($GLOBALS['worker']) && "
                          "!function_exists('check') && "
                          "!class_exists('PhpCxxChild', false)")) {
        return false;
    }
    return evaluate_boolean("(function () { include '" PHPCXX_CONVERSION_CHECK_PATH "'; "
                            "$ok = include '" PHPCXX_OBJECT_CHECK_PATH "'; "
                            "$GLOBALS['phpcxx_request_marker'] = true; "
                            "return $ok && phpcxx_probe('pending'); })()");
}

auto cleanup_balanced() -> bool
{
    auto const value = counts();
    return value.live == 0 && value.constructed == value.destroyed;
}

auto leave_pending_exception() -> void
{
    // Install an owned exception directly for engine-state isolation testing.
    // Throwing without a PHP frame would instead trigger a fatal bailout.
    zval slot{};
    object_init_ex(&slot, zend_ce_exception);
    EG(exception) = Z_OBJ(slot);
}

} // namespace phpcxx_test
