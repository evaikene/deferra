#include "thread_context.hpp"

namespace jb::core {

thread_local ThreadCtx ThreadCtx::s_ctx = ThreadCtx();

} // namespace jb::core
