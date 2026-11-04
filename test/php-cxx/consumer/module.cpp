#include <phpcxx/build.hpp>
#include <phpcxx/zend.hpp>

namespace {

zend_result start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number)
{
    const auto& info = php::build_info();
    if (info.module_api != ZEND_MODULE_API_NO || info.debug != (ZEND_DEBUG != 0) || info.zts != (USING_ZTS != 0)) {
        return FAILURE;
    }
    return SUCCESS;
}

zend_module_entry module{.size                = sizeof(zend_module_entry),
                         .zend_api            = ZEND_MODULE_API_NO,
                         .zend_debug          = ZEND_DEBUG,
                         .zts                 = USING_ZTS,
                         .name                = "phpcxx_consumer",
                         .module_startup_func = start_module,
                         .version             = "0.1.0",
                         .build_id            = ZEND_MODULE_BUILD_ID};

} // namespace

extern "C" ZEND_DLEXPORT zend_module_entry* get_module()
{
    return &module;
}
