#pragma once

/*
 * bbq.h - Lock-free block-based bounded queue (BBQ)
 *
 * This file contains a C++ implementation of the
 * Block-based Bounded Queue (BBQ) algorithm as described in:
 *
 *   "BBQ: A Block-based Bounded Queue for Exchanging Data and Profiling"
 *   Jiawei Wang et al., USENIX ATC 2022
 *   https://www.usenix.org/conference/atc22/presentation/wang-jiawei
 *
 * This implementation provides a lock-free, fixed-size queue that splits
 * the internal data structure into blocks, operated using queue- and block-level
 * control variables. The implementation supports single and multiple producer-consumer
 * scenarios. Furthermore, it can operate in two modes: RETRY_NEW and DROP_OLD.
 * For in-depth details on the algorithm, please refer to the paper.
 * Additional design considerations and implementation notes are provided below.
 *
 * Credits:
 *   Algorithm & Design: Wang et al. (2022)
 *   Implementation: Martin Georgiev <martin.r.georgiev@hotmail.com>
 *
 * Copyright (c) 2025 Martin Georgiev
 * Licensed under the MIT License.
 */

/*
 * Notes:
 * - This implementation was done first and foremost for educational purposes.
 *   As a result, the code is extensively commented to explain the algorithm
 *   and its design.
 * - The queue-level control variables (C.Head and P.Head) contain two
 *   bit segments: index and version.
 *   - The index points to the block the producer(s) and consumer(s)
 *     are currently processing (or last processed).
 *   - The version identifies how many times the internal data structure
 *     in the queue has been reused. Used to avoid the ABA problem.
 * - The block-level control variables (allocated, committed, reserved, consumed)
 *   contain two bit segments: offset and version.
 *   - The offset points to the entry in the block producer(s) and consumer(s)
 *     are currently processing (or last processed).
 *   - The version identifies how many times the block has been reused.
 *     Used to avoid the ABA problem.
 * - The algorithm design allows for overflows to the block-level cursors
 *   (i.e., the offset exceeds the block size).
 * - The first block's version starts at 0; all other blocks start at 1.
 *   The off-by-one version offset difference is constant across the queue's lifetime.
 */

/*
 * Design Considerations:
 * - Constructor parameters are used instead of template parameters to allow for
 *   the initialization of a queue, which size is only known at runtime.
 *   - Consequently, internal data structures can only be allocated on the heap,
 *     which may lead to some performance degradation due to increased likelihood
 *     of cache misses.
 * - The queue does not support resizing. The responsibility is offset to the user,
 *   who would need to copy the contents of the old queue to a newly created one.
 * - The control variables are stored as 64-bit integers, with its bit segments
 *   accessible using bitwise operations. I opted to use bitwise operations
 *   instead of bit fields or bitsets to allow for greater portability and
 *   flexibility. Bit fields are implementation-defined and may not operate as expected
 *   across different compilers or platforms. Performance-wise, bitwise operations
 *   are comparable to bit fields.
 *
 *   Refs:
 *   - https://en.cppreference.com/w/cpp/language/bit_field
 *   - https://en.cppreference.com/w/cpp/language/bitset
 *   - https://stackoverflow.com/q/2127310
 */

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <type_traits>

#ifndef ATTR_ALWAYS_INLINE
    #if defined(__GNUC__) || defined(__clang__)
        #define ATTR_ALWAYS_INLINE inline __attribute__((always_inline))
    #elif defined(_MSC_VER)
        #define ATTR_ALWAYS_INLINE __forceinline
    #else
        #define ATTR_ALWAYS_INLINE inline
    #endif
#endif

#ifndef BUILTIN_PREFETCH
    #if defined(__GNUC__) || defined(__clang__)
        #define BUILTIN_PREFETCH(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))
    #else
        #define BUILTIN_PREFETCH(addr, rw, locality) ((void)0)
    #endif
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
    BlockBoundedQueue(const std::size_t block_num, const std::size_t block_size)
        : block_num_(block_num)
        , block_size_(block_size)
        , queue_capacity_(block_num * block_size)
        , idx_bits_((block_num > 1) ? static_cast<uint8_t>(std::ceil(std::log2(block_num))) : 1)
        , off_bits_((block_size > 1) ? static_cast<uint8_t>(std::ceil(std::log2(block_size))) + 1 : 1)
        , vsn_bits_(64 - std::max(idx_bits_, off_bits_))
        , idx_mask_((idx_bits_ < 64) ? ((1ULL << idx_bits_) - 1) : ~0ULL)
        , off_mask_((off_bits_ < 64) ? ((1ULL << off_bits_) - 1) : ~0ULL)
        , vsn_mask_((vsn_bits_ < 64) ? ((1ULL << vsn_bits_) - 1) : ~0ULL)
        , blocks_(std::make_unique<Block[]>(block_num))
        , ph_(0)
        , ch_(0)
    {
        assert(block_num > 0 && block_size > 0 && "Block number and size must be greater than 0.");

        // Allocate and initialize blocks.
        for (size_t i = 0; i < block_num; ++i)
            blocks_[i].init(this, block_size, i == 0);
    }

    /// @brief Destroys the Block Bounded Queue object, releasing all allocated resources.
    ~BlockBoundedQueue() = default;

    /**
     * @brief Enqueues an element into the queue.
     *
     * @param data The data to be enqueued.
     * @return OpStatus The result of the enqueue operation.
     */
    OpStatus enqueue(const T& data)
    {
    loop:
        auto [ph, blk] = get_phead_and_block();
        EntryDesc entry(blk);
        State state = allocate_entry(blk, entry);
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
                    [[unlikely]] default:
                        std::fprintf(stderr, "Unexpected state in enqueue: %d\n", static_cast<int>(state));
                        return OpStatus::BUSY; // Fallback status
                }
            [[unlikely]] default:
                std::fprintf(stderr, "Unexpected state in enqueue: %d\n", static_cast<int>(state));
                return OpStatus::BUSY; // Fallback status
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
        EntryDesc entry(blk);
        State state = reserve_entry(blk, entry);
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
            [[unlikely]] default:
                std::fprintf(stderr, "Unexpected state in dequeue: %d\n", static_cast<int>(state));
                return {std::nullopt, OpStatus::BUSY}; // Fallback status
        }
    }

    /// @brief Checks if the queue is empty.
    /// @note Pending operations are ignored in this check.
    /// Entries allocated but not yet committed are not considered part of the queue.
    /// Reserved entries awaiting consumption still count as present in the queue.
    /// @return True if the queue is empty, false otherwise.
    bool empty() const noexcept
    {
        Block* blk = get_phead_and_block().second;

        // Load consumed first to prevent it from advancing past committed
        std::size_t consumed = blk->consumed.load(std::memory_order_acquire);
        std::size_t committed = blk->committed.load(std::memory_order_acquire);
        return (cursor_off(consumed) == cursor_off(committed)) && (cursor_vsn(consumed) == cursor_vsn(committed));
    }

    ATTR_ALWAYS_INLINE std::size_t capacity() const noexcept { return queue_capacity_; }

private:
    static constexpr std::size_t CACHELINE_SIZE = std::hardware_destructive_interference_size;

    /// @brief The block abstraction used in the queue to split the queue's entries.
    struct Block
    {
        Block() : entries(nullptr), allocated(0), committed(0), reserved(0), consumed(0) {}

        void init(const BlockBoundedQueue* parent, const std::size_t num_entries, const bool is_first)
        {
            entries = std::make_unique<T[]>(num_entries);

            // If not the first block, set the cursors' offset to the block size.
            if (!is_first)
            {
                allocated.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                committed.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                reserved.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
                consumed.store(parent->pkg_cursor(num_entries, 0), std::memory_order_relaxed);
            }
        }

        /// @brief A producer cursor pointing to the last entry in the block allocated for writing.
        /// @remark If the allocated and committed cursors point to different entries in the block,
        /// a write operation is still in progress.
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> allocated;

        /// @brief A producer cursor pointing to the last entry in the block that has been committed.
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> committed;

        /// @brief A consumer cursor pointing to the last entry in the block reserved for consumption.
        /// @remark If the reserved and consumed cursors point to different entries in the block,
        /// a read operation is still in progress.
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> reserved;

        /// @brief A consumer cursor pointing to the last entry in the block that has been consumed.
        alignas(CACHELINE_SIZE) std::atomic<std::size_t> consumed;

        std::unique_ptr<T[]> entries;
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

    /// @brief The number of blocks in the queue.
    const std::size_t block_num_;

    /// @brief The size of each block (number of entries).
    /// @remark For the total queue size, multiply this by the number of blocks.
    const std::size_t block_size_;

    /// @brief The number of elements the queue can store
    const std::size_t queue_capacity_;

    /// @brief The number of bits in the cursor allocated for the block index.
    const uint32_t idx_bits_;

    /// @brief The number of bits in the cursor allocated for the offset.
    const uint32_t off_bits_;

    /// @brief The number of bits in the control variables allocated for the version.
    const uint32_t vsn_bits_;

    /// @brief The mask for the block index in the queue-level head value.
    const std::size_t idx_mask_;

    /// @brief The mask for the offset in the block-level cursor value.
    const std::size_t off_mask_;

    /// @brief The mask for the version in the control variables.
    const std::size_t vsn_mask_;

    /// @brief The internal queue block array.
    std::unique_ptr<Block[]> blocks_;

    /// @brief The queue-level producer head (P.Head).
    alignas(CACHELINE_SIZE) std::atomic<std::size_t> ph_;

    /// @brief The queue-level consumer head (C.Head).
    alignas(CACHELINE_SIZE) std::atomic<std::size_t> ch_;

    /// @brief Gets the index from the queue-level head value (P.Head or C.Head).
    /// @param h_val The loaded atomic value of the head.
    /// @return The block index the head points to.
    ATTR_ALWAYS_INLINE constexpr std::size_t block_idx(const std::size_t h_val) const noexcept
    {
        return h_val & idx_mask_;
    }

    /// @brief Gets the version from the queue-level head value (P.Head or C.Head).
    /// @param h_val The loaded atomic value of the head.
    /// @return The version of the block the head points to.
    ATTR_ALWAYS_INLINE constexpr std::size_t block_vsn(const std::size_t h_val) const noexcept
    {
        return (h_val >> idx_bits_) & vsn_mask_;
    }

    /// @brief Packs the segment index and version back into a single value.
    /// @param idx The index of the the head.
    /// @param vsn The version of the the head.
    /// @return The packed value containing the index and version.
    ATTR_ALWAYS_INLINE constexpr std::size_t pkg_head(const std::size_t idx, const std::size_t vsn) const noexcept
    {
        return (idx & idx_mask_) | ((vsn << idx_bits_) & (vsn_mask_ << idx_bits_));
    }

    /// @brief Gets the offset from the block cursor value
    /// (i.e., allocated, committed, reserved, or consumed).
    /// @param c_val The loaded atomic value of the cursor.
    /// @return The offset of the entry in the block.
    ATTR_ALWAYS_INLINE constexpr std::size_t cursor_off(const std::size_t c_val) const noexcept
    {
        return c_val & off_mask_;
    }

    /// @brief Gets the version from the block cursor value
    /// (i.e., allocated, committed, reserved, or consumed).
    /// @param c_val The loaded atomic value of the cursor.
    ATTR_ALWAYS_INLINE constexpr std::size_t cursor_vsn(const std::size_t c_val) const noexcept
    {
        return (c_val >> off_bits_) & vsn_mask_;
    }

    /// @brief Packs the segment offset and version back into a single value.
    /// @param off The offset of the cursor in the block.
    /// @param vsn The version of the cursor in the block.
    ATTR_ALWAYS_INLINE constexpr std::size_t pkg_cursor(const std::size_t off, const std::size_t vsn) const noexcept
    {
        return (off & off_mask_) | ((vsn << off_bits_) & (vsn_mask_ << off_bits_));
    }

    /// @brief Perform an atomic maximum operation on the provided atomic variable.
    /// @remark This is a pseudo ARM MAX atomic operation implemented using CAS.
    /// @tparam U The type of the atomic variable. Must be an integral or floating-point type.
    /// @param var The atomic variable to perform the maximum operation on.
    /// @param new_val The new value to compare and potentially set as the maximum.
    /// @return The old value of the atomic variable before the operation.
    template<typename U, typename = std::enable_if_t<std::is_integral_v<U> || std::is_floating_point_v<U>>>
    static U atomic_max(std::atomic<U>& var, const U new_val) noexcept
    {
        U old_val = var.load(std::memory_order_acquire);
        if (old_val >= new_val)
            return old_val;

        while (old_val < new_val &&
               !var.compare_exchange_weak(old_val, new_val, std::memory_order_acq_rel, std::memory_order_acquire))
        {
        }
        return old_val;
    }

    ATTR_ALWAYS_INLINE const std::pair<std::size_t, Block*> get_phead_and_block() const noexcept
    {
        const std::size_t ph_val = ph_.load(std::memory_order_acquire);
        Block* blk = blocks_.get() + block_idx(ph_val);
        BUILTIN_PREFETCH(blk, 0, 2);

        return {ph_val, blk};
    }

    ATTR_ALWAYS_INLINE const std::pair<std::size_t, Block*> get_chead_and_block() const noexcept
    {
        const std::size_t ch_val = ch_.load(std::memory_order_acquire);
        Block* blk = blocks_.get() + block_idx(ch_val);
        BUILTIN_PREFETCH(blk, 0, 2);

        return {ch_val, blk};
    }

    const State allocate_entry(Block* blk, EntryDesc& entry) noexcept
    {
        const std::size_t allocated = blk->allocated.load(std::memory_order_acquire);
        if (cursor_off(allocated) >= block_size_) [[unlikely]]
            return State::BLOCK_DONE;

        const std::size_t old_cursor = blk->allocated.fetch_add(1, std::memory_order_acq_rel);
        if (cursor_off(old_cursor) >= block_size_) [[unlikely]]
            return State::BLOCK_DONE;

        entry.offset = cursor_off(old_cursor);
        return State::ALLOCATED;
    }

    ATTR_ALWAYS_INLINE void commit_entry(EntryDesc& entry, T data) noexcept
    {
        entry.block->entries[entry.offset] = std::move(data);
        entry.block->committed.fetch_add(1, std::memory_order_release);
    }

    State advance_phead(std::size_t ph) noexcept
    {
        const std::size_t nblk_idx = (block_idx(ph) + 1) % block_num_;
        const std::size_t nblk_vsn = block_vsn(ph) + (nblk_idx == 0 ? 1 : 0);
        Block* nblk = blocks_.get() + nblk_idx;
        BUILTIN_PREFETCH(nblk, 0, 2);

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            // In retry-new mode, we can only advance the P.Head to the next block
            // if the next block is fully consumed.
            const std::size_t consumed = nblk->consumed.load(std::memory_order_acquire);
            if (cursor_vsn(consumed) < block_vsn(ph) ||
                (cursor_vsn(consumed) == block_vsn(ph) && cursor_off(consumed) != block_size_))
            {
                const std::size_t reserved = nblk->reserved.load(std::memory_order_acquire);
                return (cursor_off(reserved) == cursor_off(consumed)) ? State::NO_ENTRY : State::NOT_AVAILABLE;
            }
        }
        else
        {
            // In drop-old mode, we can always advance the P.Head to the next block
            // as long as the next block is fully committed.
            const std::size_t committed = nblk->committed.load(std::memory_order_acquire);
            if (cursor_vsn(committed) == block_vsn(ph) && cursor_off(committed) < block_size_) [[unlikely]]
                return State::NOT_AVAILABLE;
        }

        atomic_max(nblk->committed, pkg_cursor(0, block_vsn(ph) + 1));
        atomic_max(nblk->allocated, pkg_cursor(0, block_vsn(ph) + 1));
        atomic_max(ph_, pkg_head(nblk_idx, nblk_vsn));

        return State::SUCCESS;
    }

    State reserve_entry(Block* blk, EntryDesc& entry) noexcept
    {
    again:
        const std::size_t reserved = blk->reserved.load(std::memory_order_acquire);
        if (cursor_off(reserved) < block_size_) [[likely]]
        {
            // Check if all committed entries have already been reserved for consumption.
            const std::size_t committed = blk->committed.load(std::memory_order_acquire);
            if (cursor_off(reserved) == cursor_off(committed))
                return State::NO_ENTRY;

            // If the committed cursor is not at the end of the block,
            // check if there are any pending allocation operations.
            if (cursor_off(committed) != block_size_)
            {
                const std::size_t allocated = blk->allocated.load(std::memory_order_acquire);
                if (cursor_off(allocated) != cursor_off(committed))
                    return State::NOT_AVAILABLE;
            }

            // Try to reserve the next entry in the block.
            if (atomic_max(blk->reserved, pkg_cursor(cursor_off(reserved) + 1, cursor_vsn(reserved))) == reserved)
            {
                entry.offset = cursor_off(reserved);
                entry.version = cursor_vsn(reserved);
                return State::RESERVED;
            }
            else
                goto again; // Retry if the reservation failed.
        }

        entry.version = cursor_vsn(reserved);
        return State::BLOCK_DONE;
    }

    [[nodiscard]] std::optional<T> consume_entry(EntryDesc& entry) noexcept
    {
        T data = std::move(entry.block->entries[entry.offset]);

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            // In retry-new mode, we can safely consume the entry as long as
            // the reserve operation was successful.
            entry.block->consumed.fetch_add(1, std::memory_order_release);
        }
        else
        {
            // In drop-old mode, we need to ensure that the entry is still valid
            // by checking the version of the allocated cursor. It is possible
            // that the entry was overwritten by a producer after it was reserved.
            const std::size_t allocated = entry.block->allocated.load(std::memory_order_acquire);
            if (cursor_vsn(allocated) != entry.version) [[unlikely]]
                return std::nullopt;
        }

        return data;
    }

    bool advance_chead(std::size_t ch, std::size_t version) noexcept
    {
        const std::size_t nblk_idx = (block_idx(ch) + 1) % block_num_;
        const std::size_t nblk_vsn = block_vsn(ch) + (nblk_idx == 0 ? 1 : 0);
        Block* nblk = blocks_.get() + nblk_idx;
        BUILTIN_PREFETCH(nblk, 0, 2);

        const std::size_t committed = nblk->committed.load(std::memory_order_acquire);

        if constexpr (mode == QueueMode::RETRY_NEW)
        {
            // In retry-new mode, the consumer can only advance the head
            // to the next block if it is fully committed.
            if (cursor_vsn(committed) != block_vsn(ch) + 1)
                return false;

            atomic_max(nblk->consumed, pkg_cursor(0, block_vsn(ch) + 1));
            atomic_max(nblk->reserved, pkg_cursor(0, block_vsn(ch) + 1));
        }
        else
        {
            // Version check to maintain FIFO order as producers may have
            // overwritten entries in the next block.
            if (cursor_vsn(committed) < version + (block_idx(ch) == 0))
                return false;

            atomic_max(nblk->reserved, pkg_cursor(0, cursor_vsn(committed)));
        }

        atomic_max(ch_, pkg_head(nblk_idx, nblk_vsn));
        return true;
    }
};

} // namespace queues