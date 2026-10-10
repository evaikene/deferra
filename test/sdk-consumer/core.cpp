#include "core_check.hpp"

#include <jb/core/json.hpp>
#include <jb/core/object.hpp>

namespace {

class Source final : public jb::core::Object {
public:
    jb::core::Signal<int> changed;

    void publish(int value) { emit(changed, value); }
};

} // namespace

auto check_core_sdk() -> int
{
    // JSON calls pull the compiled implementation into both the executable and
    // shared-library consumers, rather than merely checking inline declarations.
    auto parsed = jb::core::parse_json("{\"answer\":42}");
    if (!parsed) {
        return 1;
    }
    auto serialized = jb::core::serialize_json(parsed.value());
    if (!serialized || serialized.value() != "{\"answer\":42}") {
        return 2;
    }

    int              observed = 0;
    jb::core::Object receiver;
    Source           source;
    source.changed.connect(&receiver, [&](int value) { observed = value; });
    source.publish(42);
    return observed == 42 ? 0 : 3;
}
