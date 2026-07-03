// ReSharper disable CppTooWideScopeInitStatement
#include <Api/Dataset.hpp>

static float DistVet(const Query a, const Query b) {
    const auto diff{ a - b };
    return std::simd::reduce(diff * diff);
}

template<class T>
static void InsertSorted(T& vecs, const typename T::value_type newVal) {
    if (newVal >= vecs.back()) [[likely]]
        return;

    vecs.back() = newVal;
    for (auto i{ vecs.size() - 1 }; i != 0; --i) {
        const auto a{ vecs[i] };
        const auto b{ vecs[i - 1] };

        vecs[i]     = std::max(a, b);
        vecs[i - 1] = std::min(a, b);
    }
}


unsigned int Search(const Query input) {
    constexpr uint32_t ClusterCount{ 11 };

    struct ClusterReg {
        [[maybe_unused]] float dist{ std::numeric_limits<float>::max() };
        [[maybe_unused]] uint32_t clusterStarts{};
        [[maybe_unused]] uint32_t clusterEnds{};

        constexpr auto operator<=>(const ClusterReg& rhs) const noexcept = default;
    };

    struct VectorDist {
        [[maybe_unused]] float dist{  std::numeric_limits<float>::max()  };
        [[maybe_unused]] bool legit{};

        constexpr auto operator<=>(const VectorDist& rhs) const noexcept = default;
    };


    const auto clusters{ std::views::zip(Database.centroids, Database.clusterStarts, Database.clusterEnds) };
    auto bestClusters{ std::array<ClusterReg, ClusterCount>{} };

    for (auto [vector, startIdx, endIdx] : clusters) {
        const float dist{ DistVet(input, vector) };

        InsertSorted(bestClusters, { dist, startIdx, endIdx });
    }

    auto top5{ std::array<VectorDist, 5>() };

    for (const auto [_, start, end] : bestClusters) {
        const auto vectorSpan{ Database.features.subspan(start, end - start) };
        const auto legitSpan{ Database.isLegit.subspan(start, end - start) };

        for (auto [vec, isLegit] : std::views::zip(vectorSpan, legitSpan)) {
            const float dist{ DistVet(input, vec) };

            if (dist >= top5.back().dist)
                continue;

            InsertSorted(top5, { dist, isLegit });
        }
    }

    return std::ranges::count(top5, false, &VectorDist::legit);
}