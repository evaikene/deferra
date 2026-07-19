#pragma once

#include "query.hpp"

#include "driver_query.hpp"

#include <optional>
#include <string>
#include <vector>

namespace jb::db {

enum class BindingMode : std::uint8_t {
    None,
    Positional,
    Named,
};

struct Query::Private {
    explicit Private(Database& value)
        : database{&value}
    {}

    void reset_execution()
    {
        active          = false;
        valid           = false;
        select          = false;
        at_end          = false;
        rows_affected   = -1;
        record_metadata = {};
        current_record  = {};
    }

    Database*                         database{nullptr};
    std::unique_ptr<DriverQuery>      driver_query;
    std::string                       sql;
    std::vector<std::string>          parameter_names;
    std::vector<std::optional<Value>> bindings;
    BindingMode                       binding_mode{BindingMode::None};
    std::size_t                       next_bind_position{0};
    bool                              active{false};
    bool                              valid{false};
    bool                              select{false};
    bool                              at_end{false};
    std::int64_t                      rows_affected{-1};
    Record                            record_metadata;
    Record                            current_record;
    std::optional<jb::core::Error>    last_error;
};

} // namespace jb::db
