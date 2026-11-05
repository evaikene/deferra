#include <phpcxx/zend.hpp>

#include <catch2/catch_session.hpp>
#include <sapi/embed/php_embed.h>

#include <cstdio>

#ifdef PHPCXX_CONVERSION_TEST
#  include "consumer/functions.hpp"

namespace {
auto (*embed_startup)(sapi_module_struct*) -> int = nullptr;

auto start_conversion_sapi(sapi_module_struct* module) noexcept -> int
{
    // Embed installs its own additional_functions during init. Replace that
    // test-only list at the startup callback, before Zend registers metadata.
    module->additional_functions = phpcxx_test_functions();
    return embed_startup(module);
}
} // namespace
#endif

auto main(int argc, char** argv) -> int
{
    // Zend-dependent tests share one initialized request. Each test keeps all
    // owners local, so their destructors run before Embed tears that request down.
    char  program[]                 = "php-cxx-ownership-test";
    char* php_arguments[]           = {program, nullptr};
    php_embed_module.php_ini_ignore = 1;
#ifdef PHPCXX_CONVERSION_TEST
    // Register generated arginfo at SAPI startup, where Zend owns its persistent
    // metadata. Request-time registration has different shutdown ownership rules.
    embed_startup            = php_embed_module.startup;
    php_embed_module.startup = start_conversion_sapi;
#endif
    if (php_embed_init(1, php_arguments) == FAILURE) {
        std::fputs("PHP Embed initialization failed\n", stderr);
        return 1;
    }

    int const result = Catch::Session().run(argc, argv);
    php_embed_shutdown();
    return result;
}
