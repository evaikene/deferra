#include <phpcxx/zend.hpp>

#include <catch2/catch_session.hpp>
#include <sapi/embed/php_embed.h>

#include <cstdio>

auto main(int argc, char** argv) -> int
{
    // Zend-dependent tests share one initialized request. Each test keeps all
    // owners local, so their destructors run before Embed tears that request down.
    char  program[]                 = "php-cxx-ownership-test";
    char* php_arguments[]           = {program, nullptr};
    php_embed_module.php_ini_ignore = 1;
    if (php_embed_init(1, php_arguments) == FAILURE) {
        std::fputs("PHP Embed initialization failed\n", stderr);
        return 1;
    }

    int const result = Catch::Session().run(argc, argv);
    php_embed_shutdown();
    return result;
}
