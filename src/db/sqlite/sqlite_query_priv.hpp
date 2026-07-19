#pragma once

#include "driver_query.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::db::sqlite {

class Query final : public jb::db::DriverQuery {
public:
    explicit Query(sqlite3* connection) noexcept;
    ~Query() override;

private:
    [[nodiscard]] auto prepare(std::string_view sql) -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto parameter_count() const noexcept -> std::size_t override;
    [[nodiscard]] auto parameter_name(std::size_t index) const -> std::string_view override;
    [[nodiscard]] auto bind(std::size_t index, Value const& value) -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto exec() -> jb::core::Result<ExecutionInfo, jb::core::Error> override;
    [[nodiscard]] auto next() -> jb::core::Result<std::optional<Record>, jb::core::Error> override;
    [[nodiscard]] auto finish() -> jb::core::Result<void, jb::core::Error> override;
    void               clear() noexcept override;

    [[nodiscard]] auto metadata() const -> jb::core::Result<Record, jb::core::Error>;
    [[nodiscard]] auto current_record() const -> jb::core::Result<Record, jb::core::Error>;

    sqlite3*                 _connection{nullptr};
    sqlite3_stmt*            _statement{nullptr};
    std::vector<std::string> _parameter_names;
    bool                     _produces_records{false};
};

} // namespace jb::db::sqlite
