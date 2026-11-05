#include <phpcxx/exception.hpp>

#include <Zend/zend_exceptions.h>

#include <cassert>

namespace php {

auto has_exception() noexcept -> bool
{
    return EG(exception) != nullptr;
}

auto pending_exception() -> Error
{
    assert(has_exception());
    return {.kind = ErrorKind::PendingException, .message = {}};
}

auto raise(Error const& error, zend_class_entry* exception_class) -> void
{
    // A secondary failure must not replace or chain the original PHP exception.
    if (has_exception() || error.kind == ErrorKind::PendingException) {
        return;
    }

    switch (error.kind) {
        case ErrorKind::Type:
            if (error.argument != 0) {
                zend_argument_type_error(error.argument, "%s", error.message.c_str());
            }
            else {
                zend_type_error("%s", error.message.c_str());
            }
            break;
        case ErrorKind::Value:
            if (error.argument != 0) {
                zend_argument_value_error(error.argument, "%s", error.message.c_str());
            }
            else {
                zend_value_error("%s", error.message.c_str());
            }
            break;
        case ErrorKind::ArgumentCount:
            zend_throw_exception(zend_ce_argument_count_error, error.message.c_str(), 0);
            break;
        case ErrorKind::Runtime:
            zend_throw_exception(exception_class ? exception_class : zend_ce_exception, error.message.c_str(), 0);
            break;
        case ErrorKind::PendingException:
            break;
    }
}

} // namespace php
