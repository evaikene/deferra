#include "statistics_service.hpp"

#include <type_traits>

int main()
{
    static_assert(!std::is_copy_constructible_v<jb::jobu::StatisticsService>);
    return 0;
}
