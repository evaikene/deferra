#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)


#include "event_loop_backend.hpp"

#error "jb::core::priv::make_backend(): kqueue event loop backend is not implemented on this platform."

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__
