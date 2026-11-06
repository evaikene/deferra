#include <phpcxx/arguments.hpp>
#include <phpcxx/build.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/zend.hpp>

extern "C" {
#include <Zend/zend_enum.h> // IWYU pragma: keep generated enum registration
#include <Zend/zend_exceptions.h>
#include <ext/standard/info.h>
}

#include <string_view>

#include "jobu_arginfo.h"

namespace {

auto start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // Zend checks the module's host ABI. Also check the independently compiled
    // helper archive before installing any classes into this module lifetime.
    auto const& info = php::build_info();
    if (info.module_api != ZEND_MODULE_API_NO || info.debug != (ZEND_DEBUG != 0) || info.zts ||
        info.zend_long_size != sizeof(zend_long)) {
        return FAILURE;
    }

    register_class_JobU_QueueState();
    register_class_JobU_JobState();
    register_class_JobU_RunState();
    register_class_JobU_AttemptState();
    register_class_JobU_AttemptOutcome();
    register_class_JobU_RetryMode();
    register_class_JobU_RecoveryPolicy();
    register_class_JobU_JobType();
    register_class_JobU_OutputChannel();

    // Zend owns these persistent class entries. Do not cache them across a full
    // runtime shutdown/restart or register persistent classes during RINIT.
    auto* exception = register_class_JobU_Exception(zend_ce_exception);
    register_class_JobU_ConnectionException(exception);
    register_class_JobU_TimeoutException(exception);
    register_class_JobU_NotFoundException(exception);
    register_class_JobU_RevisionConflictException(exception);
    register_class_JobU_InvalidCursorException(exception);
    register_class_JobU_UnsavedChangesException(exception);
    register_class_JobU_OutcomeUnknownException(exception);
    register_class_JobU_UnsupportedFeatureException(exception);
    return SUCCESS;
}

auto stop_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // All module-lifetime data is owned by Zend; there are no native globals.
    return SUCCESS;
}

auto start_request([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // The skeleton has no request resources. Session ownership belongs to the
    // next stage and must be introduced at this boundary, never in MINIT.
    return SUCCESS;
}

auto stop_request([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    return SUCCESS;
}

auto module_info([[maybe_unused]] zend_module_entry* entry) noexcept -> void
{
    php_info_print_table_start();
    php_info_print_table_row(2, "JobU support", "enabled");
    php_info_print_table_row(2, "JobU extension version", JOBU_PHP_VERSION);
    php_info_print_table_row(2, "PHP build version", PHP_VERSION);
    php_info_print_table_row(2, "Thread mode", "NTS");
    php_info_print_table_end();
}

auto read_metadata(zend_execute_data* execute_data, zval* return_value, std::string_view property) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto const           count = arguments.count(0, 0);
    if (!count) {
        php::raise(count.error());
        return;
    }

    // The internal method's declaring scope selects the base's private property
    // even when a PHP subclass declares a property with the same name.
    zval  temporary{};
    auto* value = zend_read_property(execute_data->func->common.scope,
                                     Z_OBJ(execute_data->This),
                                     property.data(),
                                     property.size(),
                                     true,
                                     &temporary);
    ZVAL_COPY(return_value, value);
}

zend_module_entry module{.size                  = sizeof(zend_module_entry),
                         .zend_api              = ZEND_MODULE_API_NO,
                         .zend_debug            = ZEND_DEBUG,
                         .zts                   = USING_ZTS,
                         .name                  = "jobu",
                         .module_startup_func   = start_module,
                         .module_shutdown_func  = stop_module,
                         .request_startup_func  = start_request,
                         .request_shutdown_func = stop_request,
                         .info_func             = module_info,
                         .version               = JOBU_PHP_VERSION,
                         .build_id              = ZEND_MODULE_BUILD_ID};

} // namespace

// Zend's generated declarations cannot carry noexcept. Delegate immediately to
// a non-throwing boundary: operational errors use PHP exceptions, and allocation
// failures remain fatal rather than unwinding through Zend's C ABI.
ZEND_METHOD(JobU_Exception, getErrorCode)
{
    read_metadata(execute_data, return_value, "_errorCode");
}

ZEND_METHOD(JobU_Exception, getCategory)
{
    read_metadata(execute_data, return_value, "_category");
}

extern "C" ZEND_DLEXPORT auto get_module() -> zend_module_entry*
{
    return &module;
}
