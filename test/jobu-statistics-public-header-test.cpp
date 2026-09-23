#include "statistics.hpp"

int main()
{
    jb::jobu::StatisticsRequest request;
    jb::jobu::StatisticsPage    page;
    return request.limit == 100 && page.groups.empty() ? 0 : 1;
}
