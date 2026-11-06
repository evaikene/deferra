#include "consumer/objects.hpp"

#include <catch2/catch_session.hpp>
#include <sapi/embed/php_embed.h>

#include <cstdio>

namespace {

auto start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    return phpcxx_test::register_classes();
}

zend_module_entry module{.size                = sizeof(zend_module_entry),
                         .zend_api            = ZEND_MODULE_API_NO,
                         .zend_debug          = ZEND_DEBUG,
                         .zts                 = USING_ZTS,
                         .name                = "phpcxx_object_fixture",
                         .module_startup_func = start_module,
                         .version             = "0.1.0",
                         .build_id            = ZEND_MODULE_BUILD_ID};

auto start_sapi(sapi_module_struct* sapi) noexcept -> int
{
    // Persistent generated class metadata belongs to MINIT, before the request.
    return php_module_startup(sapi, &module);
}

} // namespace

auto main(int argc, char** argv) -> int
{
    char  program[]                 = "php-cxx-object-test";
    char* php_arguments[]           = {program, nullptr};
    php_embed_module.php_ini_ignore = 1;
    php_embed_module.startup        = start_sapi;
    if (php_embed_init(1, php_arguments) == FAILURE) {
        std::fputs("PHP Embed initialization failed\n", stderr);
        return 1;
    }

    int const result = Catch::Session().run(argc, argv);
    php_embed_shutdown();

    // PHP globals deliberately retain fixtures past the Catch tests. Only after
    // Zend's complete request shutdown can we assert their native cleanup.
    auto const counts = phpcxx_test::counts();
    if (counts.live != 0 || counts.constructed != counts.destroyed) {
        std::fputs("Native object cleanup failed at request shutdown\n", stderr);
        return 1;
    }
    return result;
}
