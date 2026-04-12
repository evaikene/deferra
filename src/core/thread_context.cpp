#include "thread_context.hpp"

namespace df::core {

thread_local ThreadCtx ThreadCtx::s_ctx = ThreadCtx();

} // namespace df::core
