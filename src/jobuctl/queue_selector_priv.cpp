#include "queue_selector_priv.hpp"

#include "command_helpers_priv.hpp"
#include "management_json.hpp"
#include "uuid.hpp"

#include <string_view>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

auto is_queue_selector_argument(CommandLineArgument const& argument) -> bool
{
    return argument.kind() == CommandLineArgumentKind::Positional || argument.name() == "id" ||
           argument.name() == "name";
}

auto parse_queue_selector(std::span<CommandLineArgument const> arguments) -> QueueSelectorParseResult
{
    auto selected = std::optional<ParsedQueueSelector>{};
    for (auto const& argument : arguments) {
        if (!is_queue_selector_argument(argument)) {
            continue;
        }
        if (selected) {
            return {.error = "expected exactly one selector: SELECTOR, --id UUID, or --name NAME"};
        }

        auto const positional = argument.kind() == CommandLineArgumentKind::Positional;
        auto const original   = positional ? std::optional<std::string_view>{argument.token()} : option_value(argument);
        if (!original || original->empty()) {
            return {.error = "queue selector must be nonempty"};
        }

        // Interpret a positional prefix once. Alias values are already explicit and remain literal.
        auto text = *original;
        auto form = QueueSelectionForm::ExactName;
        if ((!positional && argument.name() == "id") || (positional && text.starts_with("id:"))) {
            form = QueueSelectionForm::ExplicitId;
            if (positional) {
                text.remove_prefix(3);
            }
        }
        else if (positional && text.starts_with("name:")) {
            text.remove_prefix(5);
        }
        else if (positional && Uuid::parse(text)) {
            form = QueueSelectionForm::InferredId;
        }
        if (text.empty()) {
            return {.error = "queue selector must be nonempty"};
        }

        auto selector = QueueSelector{std::string{text}};
        if (form != QueueSelectionForm::ExactName) {
            auto id = Uuid::parse(text);
            if (!id) {
                return {.error = "explicit queue ID requires a valid UUID"};
            }
            selector = std::move(id).value();
        }

        // Keep name/identity validation aligned with the public request codecs, without CLI prefixes on the wire.
        auto params = queue_selector_to_json(selector);
        if (!params) {
            return {.error = params.error().message};
        }
        selected = ParsedQueueSelector{
            .selector = std::move(selector),
            .intent   = {.form = form, .original_text = std::string{*original}}
        };
    }
    return {.selector = std::move(selected)};
}

} // namespace jb::jobuctl::detail
