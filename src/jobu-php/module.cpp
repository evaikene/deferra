#include "attempt_binding.hpp"
#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "client_binding.hpp"
#include "http_payload_binding.hpp"
#include "job_binding.hpp"
#include "page_binding.hpp"
#include "query_binding.hpp"
#include "queue_binding.hpp"
#include "request.hpp"
#include "result_binding.hpp"
#include "run_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/build.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/zend.hpp>

extern "C" {
#include <Zend/zend_attributes.h>
#include <Zend/zend_enum.h> // IWYU pragma: keep generated enum registration
#include <Zend/zend_exceptions.h>
#include <ext/standard/info.h>
}

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

// gen_stub.php emits C attribute registration with a void* hash lookup. Keep
// its output authoritative and supply only the C++ conversion at this boundary.
static auto zend_add_parameter_attribute(void* function, std::uint32_t offset, zend_string* name, std::uint32_t argc)
    -> zend_attribute*
{
    return zend_add_parameter_attribute(static_cast<zend_function*>(function), offset, name, argc);
}

#include "jobu_arginfo.h"

namespace {

// NTS-only module globals carry just the active request pointer. Zend initializes
// the slot at module startup; RSHUTDOWN clears it before native cleanup begins.
struct Globals {
    jb::php::Request* request;
};

Globals globals{};

auto initialize_globals(void* storage) noexcept -> void
{
    static_cast<Globals*>(storage)->request = nullptr;
}

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
    register_class_JobU_RunCancellationDisposition();
    register_class_JobU_AttemptState();
    register_class_JobU_AttemptOutcome();
    register_class_JobU_RetryMode();
    register_class_JobU_RecoveryPolicy();
    register_class_JobU_JobType();
    register_class_JobU_OutputChannel();
    register_class_JobU_RunOrigin();
    register_class_JobU_StatisticsGroupBy();
    register_class_JobU_OutputStatus();

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
    if (!jb::php::bind_client_classes(register_class_JobU_Duration(), register_class_JobU_Client())) {
        return FAILURE;
    }
    if (!jb::php::bind_queue_class(register_class_JobU_Queue())) {
        return FAILURE;
    }
    if (!jb::php::bind_job_class(register_class_JobU_Job()) || !jb::php::bind_run_class(register_class_JobU_Run())) {
        return FAILURE;
    }
    if (!jb::php::bind_run_summary_class(register_class_JobU_RunSummary()) ||
        !jb::php::bind_attempt_classes(register_class_JobU_Attempt(), register_class_JobU_AttemptSummary())) {
        return FAILURE;
    }
    if (!jb::php::bind_page_class(register_class_JobU_Page())) {
        return FAILURE;
    }
    auto* schedule      = register_class_JobU_Schedule();
    schedule->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
    if (!jb::php::bind_value_classes(register_class_JobU_SecretReference(),
                                     register_class_JobU_OnceSchedule(schedule),
                                     register_class_JobU_CronSchedule(schedule))) {
        return FAILURE;
    }
    auto* attributes = register_class_JobU_JobAttributes();
    if (!jb::php::bind_attribute_classes(attributes,
                                         register_class_JobU_CliJobAttributes(attributes),
                                         register_class_JobU_HttpJobAttributes(attributes),
                                         register_class_JobU_QueueDefaults())) {
        return FAILURE;
    }
    if (!jb::php::bind_cli_payload_class(register_class_JobU_CliPayload())) {
        return FAILURE;
    }
    if (!jb::php::bind_http_payload_classes(register_class_JobU_HttpPayload(), register_class_JobU_HttpHeader())) {
        return FAILURE;
    }
    if (!jb::php::bind_query_classes(register_class_JobU_TimeRange(),
                                     register_class_JobU_RunQuery(),
                                     register_class_JobU_StatisticsQuery())) {
        return FAILURE;
    }
    for (auto* entry : {register_class_JobU_StatisticsGroup(),
                        register_class_JobU_StatisticsRunCounts(),
                        register_class_JobU_StatisticsAttemptCounts(),
                        register_class_JobU_StatisticsCaptureCounts(),
                        register_class_JobU_StatisticsDuration(),
                        register_class_JobU_StatisticsRunnableWaitCoverage(),
                        register_class_JobU_StatisticsMetadata(),
                        register_class_JobU_StatisticsMeasurement()}) {
        if (!jb::php::snapshot::bind(entry)) {
            return FAILURE;
        }
    }
    auto* result = register_class_JobU_ExecutionResult();
    if (!jb::php::bind_result_classes(register_class_JobU_SystemInfo(),
                                      register_class_JobU_SecretMetadata(),
                                      result,
                                      register_class_JobU_CliExecutionResult(result),
                                      register_class_JobU_HttpExecutionResult(result),
                                      register_class_JobU_OutputChunk())) {
        return FAILURE;
    }
    return SUCCESS;
}

auto stop_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // Request-owned native state has already been released by RSHUTDOWN.
    return SUCCESS;
}

auto start_request([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    globals.request = std::make_unique<jb::php::Request>().release();
    return SUCCESS;
}

auto stop_request([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    delete std::exchange(globals.request, nullptr);
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
                         .globals_size          = sizeof(Globals),
                         .globals_ptr           = &globals,
                         .globals_ctor          = initialize_globals,
                         .build_id              = ZEND_MODULE_BUILD_ID};

} // namespace

auto jb::php::current_request() noexcept -> Request*
{
    return globals.request;
}

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

ZEND_METHOD(JobU_OutcomeUnknownException, getOperation)
{
    read_metadata(execute_data, return_value, "_operation");
}

ZEND_METHOD(JobU_OutcomeUnknownException, getIdempotencyKey)
{
    read_metadata(execute_data, return_value, "_idempotencyKey");
}

extern "C" ZEND_DLEXPORT auto get_module() -> zend_module_entry*
{
    return &module;
}
