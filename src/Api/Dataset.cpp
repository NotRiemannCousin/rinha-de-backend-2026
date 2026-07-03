#include <Api/Definitions.hpp>
#include <Api/Dataset.hpp>

#include <sys/mman.h>
#include <print>
#include <simd>

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// TODO: resolver o alignment dps
template<class ...Ts>
    requires (sizeof...(Ts) != 0)
static void LoadMappedSoa(const char* path, std::span<const Ts>&... elems) {
    constexpr size_t registerSize{ (sizeof(Ts) + ...) };

#pragma region Error handling

    const int fileDesc{ open(path, O_RDONLY) };

    if (fileDesc < 0) {
        std::print("aborted: can't read '{}'", path);
        std::terminate();
    }

    struct stat fileStat{};
    if (fstat(fileDesc, &fileStat) != 0) {
        std::print("aborted: can't read '{}'", path);
        std::terminate();
    }

    if (fileStat.st_size % registerSize != 0) {
        std::print("aborted: '{}' size doesn't match regSize", path);
        std::terminate();
    }

    const void* mappedPtr{ mmap(nullptr, static_cast<size_t>(fileStat.st_size), PROT_READ, MAP_SHARED, fileDesc, 0) };
    if (mappedPtr == MAP_FAILED) {
        std::print("aborted: can't map '{}'", path);
        std::terminate();
    }

#pragma endregion

    madvise(const_cast<void*>(mappedPtr), static_cast<size_t>(fileStat.st_size), MADV_WILLNEED);
    std::span fileSpan{ static_cast<const std::byte*>(mappedPtr), static_cast<size_t>(fileStat.st_size) };
    const size_t elementCount{ fileStat.st_size / registerSize };

    template for (auto& elem : std::forward_as_tuple(elems...)) {
        using Type = std::remove_cvref_t<decltype(elem)>::element_type;
        elem = std::span<Type>{ reinterpret_cast<const Type*>(fileSpan.data()), elementCount };
        fileSpan = fileSpan.subspan(elem.size_bytes());
    }

    close(fileDesc);
}

void LoadDataset() {
    LoadMappedSoa("/data/dataset.bin", Database.features , Database.isLegit);
    LoadMappedSoa("/data/indexes.bin", Database.centroids, Database.clusterStarts, Database.clusterEnds);
}