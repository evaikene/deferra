#include "process_adapter_priv.hpp"

#include <unistd.h>

namespace jb::jobu::cli::detail {

namespace {

class SystemEffectiveIdentityProbe final : public EffectiveIdentityProbe {
public:
    [[nodiscard]] auto effective_user_id() const noexcept -> std::uint64_t override
    {
        return static_cast<std::uint64_t>(::geteuid());
    }
};

} // anonymous namespace

auto make_system_identity_probe() -> std::unique_ptr<EffectiveIdentityProbe>
{
    return std::make_unique<SystemEffectiveIdentityProbe>();
}

} // namespace jb::jobu::cli::detail
