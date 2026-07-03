#pragma once
#include <Api/Definitions.hpp>

struct DatabaseType {
    std::span<const Query> features{};
    std::span<const bool>  isLegit{};

    std::span<const Query>    centroids{};
    std::span<const uint32_t> clusterStarts{};
    std::span<const uint32_t> clusterEnds{};
};

inline DatabaseType Database{};

void LoadDataset();


unsigned int Search(Query input)
    post(res: res <= 5);

