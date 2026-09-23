#include "history_service.hpp"

#include "history_cursor_priv.hpp"
#include "object_priv.hpp"

namespace jb::jobu {

struct HistoryService::Private : jb::core::priv::ObjectPrivate {
    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes_value,
            jb::core::UuidGenerator& uuid_generator,
            jb::core::TimeSource&    time_source)
        : database{database_value}
        , attributes{attributes_value}
        , cursors{uuid_generator, time_source}
    {}

    jb::db::Database&          database;
    AttributeRegistry const&   attributes;
    detail::HistoryCursorStore cursors;
    bool                       accepting{true};
};

HistoryService::HistoryService(jb::db::Database&        database,
                               AttributeRegistry const& attributes,
                               jb::core::UuidGenerator& uuid_generator,
                               jb::core::TimeSource&    time_source,
                               jb::core::Object*        parent)
    : Object(*new Private{database, attributes, uuid_generator, time_source}, parent)
{}

HistoryService::~HistoryService() = default;

void HistoryService::shutdown() noexcept
{
    auto* data      = d_ptr<Private>();
    data->accepting = false;
    data->cursors.clear();
}

} // namespace jb::jobu
