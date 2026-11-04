#include <phpcxx/build.hpp>
#include <phpcxx/zend.hpp>

int main()
{
    const auto& info = php::build_info();
    return info.version_id == PHP_VERSION_ID && info.module_api == ZEND_MODULE_API_NO &&
                   info.debug == (ZEND_DEBUG != 0) && info.zts == (USING_ZTS != 0) &&
                   info.zend_long_size == sizeof(zend_long)
             ? 0
             : 1;
}
