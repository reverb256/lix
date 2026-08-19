#pragma once
///@file

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace nix {

/**
 * Provides an indexable container like vector<> with memory overhead
 * guarantees like list<> by allocating storage in chunks of ChunkSize
 * elements instead of using a contiguous memory allocation like vector<>
 * does. Not using a single vector that is resized reduces memory overhead
 * on large data sets by on average (growth factor)/2, mostly
 * eliminates copies within the vector during resizing, and provides stable
 * references to its elements.
 *
 * Thread safety: appends (::add) are safe to run concurrently with
 * reads of already-published elements (::operator[]), provided appends
 * are themselves serialized by the caller. This is achieved by:
 *   - storing the chunk-pointer array in a fixed-size std::array that
 *     never reallocates, so a reader indexing into it is always safe;
 *   - reserving each chunk to its full capacity when it is created, so
 *     the chunk's data() pointer never moves;
 *   - publishing an element (writing it into the chunk) before handing
 *     out its index, so a reader can only ever observe fully-written
 *     elements.
 * The `size` counter is atomic so ::size() can be read from any thread.
 */
template<typename T, size_t ChunkSize>
class ChunkedVector {
private:
    static constexpr size_t MaxChunks = 1 << 16;

    std::atomic<uint32_t> size_{0};
    std::atomic<uint32_t> numChunks{0};
    std::vector<T> chunks[MaxChunks];

    /**
     * Keep this out of the ::add hot path
     */
    [[gnu::noinline]]
    auto & addChunk()
    {
        if (size_.load(std::memory_order_relaxed) >= std::numeric_limits<uint32_t>::max() - ChunkSize) {
            abort();
        }
        const auto n = numChunks.fetch_add(1, std::memory_order_relaxed);
        if (n >= MaxChunks) {
            abort();
        }
        chunks[n].reserve(ChunkSize);
        return chunks[n];
    }

public:
    ChunkedVector(uint32_t)
    {
        addChunk();
    }

    uint32_t size() const noexcept
    {
        return size_.load(std::memory_order_relaxed);
    }

    template<typename... Args>
    std::pair<T &, uint32_t> add(Args &&... args)
    {
        const auto idx = size_.fetch_add(1, std::memory_order_relaxed);
        auto & chunk = [&]() -> auto & {
            if (auto & back = chunks[numChunks.load(std::memory_order_relaxed) - 1]; back.size() < ChunkSize)
            {
                return back;
            }
            return addChunk();
        }();
        auto & result = chunk.emplace_back(std::forward<Args>(args)...);
        return {result, idx};
    }

    /**
     * Unchecked subscript operator.
     * @pre add must have been called at least idx + 1 times.
     * @throws nothing
     */
    const T & operator[](uint32_t idx) const noexcept
    {
        return chunks[idx / ChunkSize][idx % ChunkSize];
    }

    template<typename Fn>
    void forEach(Fn fn) const
    {
        for (size_t n = 0; n < numChunks.load(std::memory_order_relaxed); n++) {
            for (const auto & e : chunks[n]) {
                fn(e);
            }
        }
    }
};
}
