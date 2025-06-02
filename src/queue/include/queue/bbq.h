#pragma once

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

// Queue-level control variables
// - C.Head (points to a block in the queue)
// - P.Head (points to a block in the queue)
// - The queue-level control variables contain an index pointing to a block and
// a version identifying how many times the whole queue has been reused.

// Block-level control variables (cursors)
// - allocated (producer cursor)
// - committed (producer cursor)
// - reserved (consumer cursor)
// - consumed (consumer cursor)
// - The block-level control variables contain an offset field pointing to an
// entry in the block and a version field identifying how many times the block
// has been reused.

// Notes:
// - Producers start using a block only after it has been fully consumed.
// - Enqueue operation is supposed to use FAA (Fetch-And-Add).
// - Dequeue operation is supposed to use CAS (Compare-And-Swap) to avoid
// consumers from invalidating entries currently used by producers.
// - Enqueue steps:
//   1. Retrieve current value of P.Head and the corresponding block identifier.
//   2. An allocation is attempted on the block.
//   2a. If it succeeds, the producer writes the data into the allocated entry
//   and commits it.
//   3. If it fails, then it means that the block is full, so the producer will
//   try to advance the P.Head to the next block. 3a. If it succeeds, the
//   producer loops back to step 1. 3b. In retry-new mode, advancing P.Head will
//   fail if the next block is not fully consumed.
//   - The algorithm distinguishes between failures: It is either returns FULL
//   or BUSY if some dequeue operation is ongoing. 3c. In drop-old mode,
//   advancing P.Head will not fail except for a seldom case, for which BUSY is
//   returned.
// - Dequeue steps:
//   1. Retrieves the current value of C.Head and the corresponding block
//   identifier.
//   2. It attempts to reserve an entry in the block to consume, advancing the
//   reserved cursor. 2a. If it succeeds, the consumer reads the data from the
//   reserved entry and advances the consumed cursor. 2b. In drop-old mode, the
//   consumer may have to retry consuming if the producers have overwritten the
//   block, going back to step 1. 2c. Reserving an entry can fail in several
//   ways:
//      - When the next entry in blk is allocated but not yet commited, dequeue
//      will return BUSY.
//      - When blk is not fully allocated and all commited entries were already
//      consumed, dequeue will return EMPTY.
//   3. When blk is fully commited and fully consumed, the consumer tries to
//   advance C.Head to the next block. 3a. If it succeds, it retries to reserve
//   an entry, jumping back to step 1. 3b. If it fails, it returns EMPTY,
//   indicating that all entries in the queue have been consumed.
// - Control variables are versioned to identify multiple reuses of the same
// memory locations and avoid the ABA problem.

// Design considerations:
// - Constructor parameters are used instead of template parameters so that the
//   queue can be dynamically resized.
//   - While the ready queue size for Lingua Franca's C++ runtime is fixed, the
//     reactions and event queues do not have a fixed size. Since these
//     parameters are set and configured only once during initializaiton,
//     peformance is negligible. Source: "the ready queue is implemented as a
//     fixed-size buffer paired with an atomic counter. Since we know precisely
//     how many reactions can at most run in parallel (i.e., the maximum number
//     of reactions in the APG that have the same level), we can fix the size of
//     the queue"
// - The queue will not directly support resizing. The responsibility is offset
//   to the user, who can create a new queue with a different size and copy the
//   contents of the old queue to the new one.
// - Blocks are defined in the heap as the queue is expected to be used in a context where the number of blocks is not
//   known at compile time. This will result in some performance penalty as we'll be more prone to cache misses.
// - The index, offset and version fields are stored in a 64-bit integer and accessed using bitwise operations.
//   I opted to use bitwise operations instead of bit fields or bitsets to ensure portability. Bit fields are
//   implementation-defined and may not operate as expected across different compilers or platforms. Performance-wise,
//   bitwise operations are comparable to bit fields.
//   - Source:
//   https://stackoverflow.com/questions/2127310/c-c-bitfields-versus-bitwise-operators-to-single-out-bits-which-is-faster-be

// TODO: Consider removing copy/move constructors to avoid implicit copying of blocks/entry descriptors.
// TODO: Compare performance with/without the cache line alignments.

#if defined(__GNUC__) || defined(__clang__)
    #define BBQ_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define BBQ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define BBQ_LIKELY(x)   (x)
    #define BBQ_UNLIKELY(x) (x)
#endif

namespace queues
{

/// @brief Enum class that defines the possible queue operation OpStatus codes.
enum class OpStatus : uint8_t
{
    /// @brief The operation was successful.
    OK = 0,

    /// @brief The queue is full, and the enqueue operation cannot proceed.
    FULL = 1,

    /// @brief The queue is empty, and the dequeue operation cannot proceed.
    EMPTY = 2,

    /// @brief The queue is busy, and the operation cannot proceed due to
    /// another ongoing operation (e.g., another enqueue or dequeue).
    BUSY = 3,
};

/// @brief Enum class to define the queue's operation mode.
enum class QueueMode : uint8_t
{
    /// @brief In this mode, enqueue operations will fail if the
    /// next block is not fully consumed.
    RETRY_NEW = 0,

    /// @brief In this mode, enqueue operations will only fail if the
    /// next block has not been fully committed by pending operations
    /// with older versions.
    ///
    /// Old entries will be overwritten, even if they are not fully consumed.
    DROP_OLD = 1,
};

/**
 * @brief A lock-free fixed-size queue based on the Block-based Bounded Queue
 * (BBQ) algorithm from "BBQ: A Block-based Bounded Queue for Exchanging
 * Data and Profiling" (Wang et al., 2022).
 *
 * @tparam T The data type of the elements stored in the queue.
 * @tparam mode The operation mode of the queue. Default is RETRY_NEW.
 */
template<typename T, QueueMode mode = QueueMode::RETRY_NEW>
class BlockBoundedQueue
{
public:
    /**
     * @brief Constructs a BlockBoundedQueue with the specified number of blocks
     * and block size.
     *
     * @param block_num The number of blocks in the queue.
     * @param block_size The size of each block (number of entries).
     *
     * @remark The length of the offset needs to be larger than log2(block_size)
     * to allow for FAA-overflow detection.
     */
    BlockBoundedQueue(std::size_t block_num, std::size_t block_size)
        : block_num_(block_num)
        , block_size_(block_size)
        , idx_bits_((block_num > 1) ? static_cast<uint8_t>(std::ceil(std::log2(block_num))) : 1)
        , off_bits_((block_size > 1) ? static_cast<uint8_t>(std::ceil(std::log2(block_size))) + 1 : 1)
        , vsn_bits_(64 - std::max(idx_bits_, off_bits_))
        , idx_mask_((idx_bits_ < 64) ? ((1ULL << idx_bits_) - 1) : ~0ULL)
        , off_mask_((off_bits_ < 64) ? ((1ULL << off_bits_) - 1) : ~0ULL)
        , vsn_mask_((vsn_bits_ < 64) ? ((1ULL << vsn_bits_) - 1) : ~0ULL)
    {
        assert(block_num > 0 && block_size > 0 && "Block number and size must be greater than 0.");

        // Allocate and initialize blocks.
        blocks_ = new Block[block_num];
        for (size_t i = 0; i < block_num; ++i)
            blocks_[i].init(this, block_size, i == 0);
    }

    /// @brief Destroys the Block Bounded Queue object, releasing all allocated resources.
    ~BlockBoundedQueue() { delete[] blocks_; }

    /**
     * @brief Enqueues an element into the queue.
     *
     * @param data The data to be enqueued.
     * @return OpOpStatus The result of the enqueue operation.
     */
    OpStatus enqueue(const T& data)
    {
    loop:
        auto [ph, blk] = get_phead_and_block();
        auto [state, entry] = allocate_entry(blk);
        switch (state)
        {
            case State::ALLOCATED:
                // Successfully allocated an entry.
                commit_entry(entry, data);
                return OpStatus::OK;
            case State::BLOCK_DONE:
                switch (advance_phead(ph))
                {
                    case State::NO_ENTRY:
                        return OpStatus::FULL;
                    case State::NOT_AVAILABLE:
                        return OpStatus::BUSY;
                    case State::SUCCESS:
                        goto loop;
                    default:
                        std::fprintf(stderr, "Unexpected state in enqueue: %d\n", static_cast<int>(state));
                        return OpStatus::BUSY; // Fallback OpStatus
                }
            default:
                std::fprintf(stderr, "Unexpected state in enqueue: %d\n", static_cast<int>(state));
                return OpStatus::BUSY; // Fallback OpStatus
        }
    }

    /**
     * @brief Dequeues an element from the queue.
     *
     * @return A pair containing an optional T (the dequeued value if successful, otherwise std::nullopt)
     * and the dequeue OpStatus.
     */
    const std::pair<std::optional<T>, OpStatus> dequeue()
    {
    loop:
        auto [ch, blk] = get_chead_and_block();
        auto [state, entry] = reserve_entry(blk);
        std::optional<T> opt_data;
        switch (state)
        {
            case State::RESERVED:
                opt_data = consume_entry(entry);
                if (opt_data.has_value())
                    return {opt_data.value(), OpStatus::OK};
                else
                    goto loop;
            case State::NO_ENTRY:
                return {std::nullopt, OpStatus::EMPTY};
            case State::NOT_AVAILABLE:
                return {std::nullopt, OpStatus::BUSY};
            case State::BLOCK_DONE:
                if (advance_chead(ch, entry.version))
                    goto loop;
                else
                    return {std::nullopt, OpStatus::EMPTY};
            default:
                std::fprintf(stderr, "Unexpected state in dequeue: %d\n", static_cast<int>(state));
                return {std::nullopt, OpStatus::BUSY}; // Fallback OpStatus
        }
    }

private:
    inline static constexpr std::size_t CACHELINE_SIZE = std::hardware_destructive_interference_size;

    /// @brief The block abstraction used in the queue to split the queue's entries.
    struct Block
    {
        Block() : entries(nullptr), allocated(0), committed(0), reserved(0), consumed(0) {}

        ~Block() { delete[] entries; }

        void init(const BlockBoundedQueue* parent, const std::size_t num_entries, bool is_first)
        {
            entries = new T[num_entries];

            // If not the first block, set the cursors' offset to the block size.
            if (!is_first)
            {
                allocated.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                committed.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                reserved.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                consumed.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
            }
        }

        alignas(CACHELINE_SIZE) T* entries;
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> allocated;
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> committed;
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> reserved;
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> consumed;
    };

    /// @brief A metadata structure that describes an entry in the queue.
    struct EntryDesc
    {
        Block* block;
        std::size_t offset;
        std::size_t version;

        explicit EntryDesc(Block* block, std::size_t offset = 0, std::size_t version = 0) noexcept
            : block(block), offset(offset), version(version)
        {
        }
    };

    /// @brief Enum class that defines the possible states of operations on the
    /// internal blocks.
    enum class State : uint8_t
    {
        /// @brief A state indicating that the operation was successful.
        SUCCESS = 0,

        /// @brief A state indicating that the block has been fully processed
        BLOCK_DONE = 1,

        /// @brief A state indicating that an entry in the block has been
        /// successfully allocated.
        ALLOCATED = 2,

        /// @brief A state indicating that an entry in the block has been
        /// successfully reserved for consumption.
        RESERVED = 3,

        /// @brief A state indicating that all reserved entries in the block
        /// have been consumed, and no new entries are available.
        NO_ENTRY = 4,

        /// @brief A state indicating that the next entry in the block is not
        /// available for allocation or reservation, typically due a
        /// pending operation that is not yet complete.
        NOT_AVAILABLE = 5,
    };

    std::size_t block_num_;
    std::size_t block_size_;
    const uint8_t idx_bits_;
    const uint8_t off_bits_;
    const uint8_t vsn_bits_;
    const std::size_t idx_mask_;
    const std::size_t off_mask_;
    const std::size_t vsn_mask_;

    /// @brief The internal queue block array.
    alignas(CACHELINE_SIZE) Block* blocks_;

    /// @brief The queue-level producer head (P.Head).
    alignas(CACHELINE_SIZE) std::atomic<std::size_t> ph_;

    /// @brief The queue-level consumer head (C.Head).
    alignas(CACHELINE_SIZE) std::atomic<std::size_t> ch_;

    /// @brief Gets the index from the queue-level head value (P.Head or C.Head).
    /// @param h_val The loaded atomic value of the head.
    /// @return The block index the head points to.
    inline std::size_t block_idx(const std::size_t h_val) const noexcept { return h_val & idx_mask_; }

    /// @brief Gets the version from the queue-level head value (P.Head or C.Head).
    /// @param h_val The loaded atomic value of the head.
    /// @return The version of the block the head points to.
    inline std::size_t block_vsn(const std::size_t h_val) const noexcept { return (h_val >> idx_bits_) & vsn_mask_; }

    /// @brief Packs the segment index and version back into a single value.
    /// @param idx The index of the the head.
    /// @param vsn The version of the the head.
    /// @return The packed value containing the index and version.
    inline std::size_t pkg_head(std::size_t idx, std::size_t vsn) const noexcept
    {
        return (idx & idx_mask_) | ((vsn << idx_bits_) & (vsn_mask_ << idx_bits_));
    }

    /// @brief Gets the offset from the block cursor value
    /// (i.e., allocated, committed, reserved, or consumed).
    /// @param c_val The loaded atomic value of the cursor.
    /// @return The offset of the entry in the block.
    inline std::size_t cursor_off(const std::size_t c_val) const noexcept { return c_val & off_mask_; }

    /// @brief Gets the version from the block cursor value
    /// (i.e., allocated, committed, reserved, or consumed).
    /// @param c_val The loaded atomic value of the cursor.
    inline std::size_t cursor_vsn(const std::size_t c_val) const noexcept { return (c_val >> off_bits_) & vsn_mask_; }

    /// @brief Packs the segment offset and version back into a single value.
    /// @param off The offset of the cursor in the block.
    /// @param vsn The version of the cursor in the block.
    inline std::size_t pkg_cursor(std::size_t off, std::size_t vsn) const noexcept
    {
        return (off & off_mask_) | ((vsn << off_bits_) & (vsn_mask_ << off_bits_));
    }

    template<typename U, typename = std::enable_if_t<std::is_integral_v<U> || std::is_floating_point_v<U>>>
    U atomic_max(std::atomic<U>& var, const int new_val)
    {
        U old_val = var.load(std::memory_order_relaxed);
        if (old_val >= new_val)
            return old_val;

        while (old_val < new_val &&
               !var.compare_exchange_weak(old_val, new_val, std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }
        return old_val;
    }

    const std::pair<std::size_t, Block*> get_phead_and_block() const
    {
        std::size_t ph_val = ph_.load(std::memory_order_relaxed);
        return {ph_val, &blocks_[block_idx(ph_val)]};
    }

    const std::pair<std::size_t, Block*> get_chead_and_block() const
    {
        std::size_t ch_val = ch_.load(std::memory_order_relaxed);
        return {ch_val, &blocks_[block_idx(ch_val)]};
    }

    const std::pair<State, EntryDesc> allocate_entry(Block* blk)
    {
        std::size_t allocated = blk->allocated.load(std::memory_order_relaxed);
        if (BBQ_UNLIKELY(cursor_off(allocated) >= block_size_))
            return std::make_pair(State::BLOCK_DONE, EntryDesc(nullptr, 0, 0));

        std::size_t old_cursor = blk->allocated.fetch_add(1, std::memory_order_relaxed);
        if (BBQ_UNLIKELY(cursor_off(old_cursor) >= block_size_))
            return std::make_pair(State::BLOCK_DONE, EntryDesc(nullptr, 0, 0));

        return std::make_pair(State::ALLOCATED, EntryDesc(blk, cursor_off(old_cursor)));
    }

    void commit_entry(EntryDesc& entry, T data)
    {
        entry.block->entries[entry.offset] = std::move(data);
        entry.block->committed.fetch_add(1, std::memory_order_relaxed);
    }

    State advance_phead(std::size_t ph)
    {
        const std::size_t nblk_idx = (block_idx(ph) + 1) % block_num_;
        const std::size_t nblk_vsn = block_vsn(ph) + (nblk_idx == 0 ? 1 : 0);
        Block* nblk = &blocks_[nblk_idx];

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            // In retry-new mode, we can only advance the P.Head to the next block
            // if the next block is fully consumed.
            std::size_t consumed = nblk->consumed.load(std::memory_order_acquire);
            if (cursor_vsn(consumed) < block_vsn(ph) ||
                (cursor_vsn(consumed) == block_vsn(ph) && cursor_off(consumed) != block_size_))
            {
                std::size_t reserved = nblk->reserved.load(std::memory_order_acquire);
                return (cursor_off(reserved) == cursor_off(consumed)) ? State::NO_ENTRY : State::NOT_AVAILABLE;
            }
        }
        else
        {
            // In drop-old mode, we can always advance the P.Head to the next block
            // as long as the next block is fully committed.
            std::size_t committed = nblk->committed.load(std::memory_order_acquire);
            if (cursor_vsn(committed) == block_vsn(ph) && cursor_off(committed) < block_size_)
                return State::NOT_AVAILABLE;
        }

        atomic_max(nblk->committed, pkg_cursor(0, block_vsn(ph) + 1));
        atomic_max(nblk->allocated, pkg_cursor(0, block_vsn(ph) + 1));
        atomic_max(ph_, pkg_head(nblk_idx, nblk_vsn));

        return State::SUCCESS;
    }

    std::pair<State, EntryDesc> reserve_entry(Block* blk)
    {
    again:
        std::size_t reserved = blk->reserved.load(std::memory_order_relaxed);
        if (BBQ_LIKELY(cursor_off(reserved) < block_size_))
        {
            // Check if all committed entries have already been reserved for consumption.
            std::size_t committed = blk->committed.load(std::memory_order_relaxed);
            if (cursor_off(reserved) == cursor_off(committed))
                return std::make_pair(State::NO_ENTRY, EntryDesc(nullptr, 0, 0));

            // If the committed cursor is not at the end of the block,
            // check if there are any pending allocation operations.
            if (cursor_off(committed) != block_size_)
            {
                std::size_t allocated = blk->allocated.load(std::memory_order_relaxed);
                if (cursor_off(allocated) != cursor_off(committed))
                    return std::make_pair(State::NOT_AVAILABLE, EntryDesc(nullptr, 0, 0));
            }

            if (atomic_max(blk->reserved, pkg_cursor(cursor_off(reserved) + 1, cursor_vsn(reserved))) == reserved)
                return std::make_pair(State::RESERVED, EntryDesc(blk, cursor_off(reserved), cursor_vsn(reserved)));
            else
                goto again; // Retry if the reservation failed.
        }

        return std::make_pair(State::BLOCK_DONE, EntryDesc(blk, cursor_vsn(reserved)));
    }

    [[nodiscard]] std::optional<T> consume_entry(EntryDesc& entry)
    {
        T data = std::move(entry.block->entries[entry.offset]);

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            entry.block->consumed.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            std::size_t allocated = entry.block->allocated.load(std::memory_order_acquire);
            if (cursor_vsn(allocated) != entry.version)
                return std::nullopt;
        }

        return data;
    }

    bool advance_chead(std::size_t ch, std::size_t version)
    {
        const std::size_t nblk_idx = (block_idx(ch) + 1) % block_num_;
        const std::size_t nblk_vsn = block_vsn(ch) + (nblk_idx == 0 ? 1 : 0);
        Block* nblk = &blocks_[nblk_idx];

        std::size_t committed = nblk->committed.load(std::memory_order_acquire);

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            if (cursor_vsn(committed) != block_vsn(ch) + 1)
                return false;

            atomic_max(nblk->consumed, pkg_cursor(0, block_vsn(ch) + 1));
            atomic_max(nblk->reserved, pkg_cursor(0, block_vsn(ch) + 1));
        }
        else
        {
            if (cursor_vsn(committed) < version + (block_idx(ch) == 0))
                return false;

            atomic_max(nblk->reserved, pkg_cursor(0, cursor_vsn(committed)));
        }

        atomic_max(ch_, pkg_head(nblk_idx, nblk_vsn));
        return true;
    }
};

} // namespace queues