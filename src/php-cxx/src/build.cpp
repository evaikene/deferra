#include <phpcxx/build.hpp>
#include <phpcxx/zend.hpp>

namespace php {

const BuildInfo& build_info() noexcept
{
    static_assert(sizeof(zend_long) == 8, "php-cxx requires a 64-bit zend_long");

    static constexpr BuildInfo info{.version_id     = PHP_VERSION_ID,
                                    .module_api     = ZEND_MODULE_API_NO,
                                    .debug          = ZEND_DEBUG != 0,
                                    .zts            = USING_ZTS != 0,
                                    .zend_long_size = sizeof(zend_long)};
    return info;
}

} // namespace php
