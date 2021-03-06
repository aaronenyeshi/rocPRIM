// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_DEVICE_DETAIL_DEVICE_REDUCE_BY_KEY_HPP_
#define ROCPRIM_DEVICE_DETAIL_DEVICE_REDUCE_BY_KEY_HPP_

#include <iterator>
#include <utility>

#include "../../config.hpp"
#include "../../detail/various.hpp"

#include "../../intrinsics.hpp"
#include "../../functional.hpp"

#include "../../block/block_discontinuity.hpp"
#include "../../block/block_load.hpp"
#include "../../block/block_store.hpp"
#include "../../block/block_scan.hpp"

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

template<class Key, class Value>
struct carry_out
{
    Key key;
    Value value;
    unsigned int destination;
    unsigned int is_final_aggregate;
};

template<class Value>
struct scan_by_key_pair
{
    unsigned int key;
    Value value;
};

// Special operator which allows to calculate scan-by-key using block_scan.
// block_scan supports non-commutative scan operators.
// Initial values of pairs' keys must be 1 for the first item (head) of segment and 0 otherwise.
// As a result key contains the current segment's index and value contains segmented scan result.
template<class Pair, class BinaryFunction>
struct scan_by_key_op
{
    BinaryFunction reduce_op;

    ROCPRIM_DEVICE inline
    scan_by_key_op(BinaryFunction reduce_op)
        : reduce_op(reduce_op)
    {}

    ROCPRIM_DEVICE inline
    Pair operator()(const Pair& a, const Pair& b)
    {
        Pair c;
        c.key = a.key + b.key;
        c.value = b.key != 0
            ? b.value
            : reduce_op(a.value, b.value);
        return c;
    }
};

// Wrappers that reverse results of key comparizon functions to use them as flag_op of block_discontinuity
// (for example, equal_to will work as not_equal_to and divide items into segments by keys)
template<class Key, class KeyCompareFunction>
struct key_flag_op
{
    KeyCompareFunction key_compare_op;

    ROCPRIM_DEVICE inline
    key_flag_op(KeyCompareFunction key_compare_op)
        : key_compare_op(key_compare_op)
    {}

    ROCPRIM_DEVICE inline
    bool operator()(const Key& a, const Key& b) const
    {
        return !key_compare_op(a, b);
    }
};

// This wrapper processes only part of items and flags (valid_count - 1)th item (for tails)
// and (valid_count)th item (for heads), all items after valid_count are unflagged.
template<class Key, class KeyCompareFunction>
struct guarded_key_flag_op
{
    KeyCompareFunction key_compare_op;
    unsigned int valid_count;

    ROCPRIM_DEVICE inline
    guarded_key_flag_op(KeyCompareFunction key_compare_op, unsigned int valid_count)
        : key_compare_op(key_compare_op), valid_count(valid_count)
    {}

    ROCPRIM_DEVICE inline
    bool operator()(const Key& a, const Key& b, unsigned int b_index) const
    {
        return (b_index < valid_count && !key_compare_op(a, b)) || b_index == valid_count;
    }
};

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    class KeysInputIterator,
    class KeyCompareFunction
>
ROCPRIM_DEVICE inline
void fill_unique_counts(KeysInputIterator keys_input,
                        unsigned int size,
                        unsigned int * unique_counts,
                        KeyCompareFunction key_compare_op,
                        unsigned int blocks_per_full_batch,
                        unsigned int full_batches,
                        unsigned int blocks)
{
    constexpr unsigned int items_per_block = BlockSize * ItemsPerThread;
    constexpr unsigned int warp_size = ::rocprim::warp_size();
    constexpr unsigned int warps_no = BlockSize / warp_size;

    using key_type = typename std::iterator_traits<KeysInputIterator>::value_type;

    using keys_load_type = ::rocprim::block_load<
        key_type, BlockSize, ItemsPerThread,
        ::rocprim::block_load_method::block_load_transpose>;
    using discontinuity_type = ::rocprim::block_discontinuity<key_type, BlockSize>;

    ROCPRIM_SHARED_MEMORY struct
    {
        union
        {
            typename keys_load_type::storage_type keys_load;
            typename discontinuity_type::storage_type discontinuity;
        };
        unsigned int unique_counts[warps_no];
    } storage;

    const unsigned int flat_id = ::rocprim::flat_block_thread_id();
    const unsigned int batch_id = ::rocprim::detail::block_id<0>();
    const unsigned int lane_id = ::rocprim::lane_id();
    const unsigned int warp_id = ::rocprim::warp_id();

    unsigned int block_id;
    unsigned int blocks_per_batch;
    if(batch_id < full_batches)
    {
        blocks_per_batch = blocks_per_full_batch;
        block_id = batch_id * blocks_per_batch;
    }
    else
    {
        blocks_per_batch = blocks_per_full_batch - 1;
        block_id = batch_id * blocks_per_batch + full_batches;
    }

    unsigned int warp_unique_count = 0;

    for(unsigned int bi = 0; bi < blocks_per_batch; bi++)
    {
        const unsigned int block_offset = block_id * items_per_block;

        key_type keys[ItemsPerThread];
        unsigned int valid_count;
        if(block_offset + items_per_block <= size)
        {
            valid_count = items_per_block;
            keys_load_type().load(keys_input + block_offset, keys, storage.keys_load);
        }
        else
        {
            valid_count = size - block_offset;
            keys_load_type().load(keys_input + block_offset, keys, valid_count, storage.keys_load);
        }

        bool tail_flags[ItemsPerThread];
        key_type successor_key = keys[ItemsPerThread - 1];
        ::rocprim::syncthreads();
        if(block_id == blocks - 1)
        {
            discontinuity_type().flag_tails(
                tail_flags, successor_key, keys,
                guarded_key_flag_op<key_type, KeyCompareFunction>(key_compare_op, valid_count),
                storage.discontinuity
            );
        }
        else
        {
            if(flat_id == BlockSize - 1)
            {
                successor_key = keys_input[block_offset + items_per_block];
            }
            discontinuity_type().flag_tails(
                tail_flags, successor_key, keys,
                key_flag_op<key_type, KeyCompareFunction>(key_compare_op),
                storage.discontinuity
            );
        }

        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            warp_unique_count += ::rocprim::bit_count(::rocprim::ballot(tail_flags[i]));
        }

        block_id++;
    }

    if(lane_id == 0)
    {
        storage.unique_counts[warp_id] = warp_unique_count;
    }
    ::rocprim::syncthreads();

    if(flat_id == 0)
    {
        unsigned int batch_unique_count = 0;
        for(unsigned int w = 0; w < warps_no; w++)
        {
            batch_unique_count += storage.unique_counts[w];
        }
        unique_counts[batch_id] = batch_unique_count;
    }
}

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    class UniqueCountOutputIterator
>
ROCPRIM_DEVICE inline
void scan_unique_counts(unsigned int * unique_counts,
                        UniqueCountOutputIterator unique_count_output,
                        unsigned int batches)
{
    using load_type = ::rocprim::block_load<
        unsigned int, BlockSize, ItemsPerThread,
        ::rocprim::block_load_method::block_load_transpose>;
    using store_type = ::rocprim::block_store<
        unsigned int, BlockSize, ItemsPerThread,
        ::rocprim::block_store_method::block_store_transpose>;
    using scan_type = typename ::rocprim::block_scan<unsigned int, BlockSize>;

    ROCPRIM_SHARED_MEMORY union
    {
        typename load_type::storage_type load;
        typename store_type::storage_type store;
        typename scan_type::storage_type scan;
    } storage;

    const unsigned int flat_id = ::rocprim::flat_block_thread_id();

    unsigned int values[ItemsPerThread];
    load_type().load(unique_counts, values, batches, 0, storage.load);

    unsigned int unique_count;
    ::rocprim::syncthreads();
    scan_type().exclusive_scan(values, values, 0, unique_count);

    ::rocprim::syncthreads();
    store_type().store(unique_counts, values, batches, storage.store);

    if(flat_id == 0)
    {
        *unique_count_output = unique_count;
    }
}

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    class KeysInputIterator,
    class ValuesInputIterator,
    class UniqueOutputIterator,
    class AggregatesOutputIterator,
    class CarryOut,
    class KeyCompareFunction,
    class BinaryFunction
>
ROCPRIM_DEVICE inline
void reduce_by_key(KeysInputIterator keys_input,
                   ValuesInputIterator values_input,
                   unsigned int size,
                   const unsigned int * unique_starts,
                   CarryOut * carry_outs,
                   UniqueOutputIterator unique_output,
                   AggregatesOutputIterator aggregates_output,
                   KeyCompareFunction key_compare_op,
                   BinaryFunction reduce_op,
                   unsigned int blocks_per_full_batch,
                   unsigned int full_batches,
                   unsigned int blocks)
{
    constexpr unsigned int items_per_block = BlockSize * ItemsPerThread;

    using key_type = typename std::iterator_traits<KeysInputIterator>::value_type;
    using value_type = typename std::iterator_traits<ValuesInputIterator>::value_type;

    using keys_load_type = ::rocprim::block_load<
        key_type, BlockSize, ItemsPerThread,
        ::rocprim::block_load_method::block_load_transpose>;
    using values_load_type = ::rocprim::block_load<
        value_type, BlockSize, ItemsPerThread,
        ::rocprim::block_load_method::block_load_transpose>;
    using discontinuity_type = ::rocprim::block_discontinuity<key_type, BlockSize>;
    using scan_type = ::rocprim::block_scan<scan_by_key_pair<value_type>, BlockSize>;

    ROCPRIM_SHARED_MEMORY struct
    {
        union
        {
            typename keys_load_type::storage_type keys_load;
            typename values_load_type::storage_type values_load;
            typename discontinuity_type::storage_type discontinuity;
            typename scan_type::storage_type scan;
        };
        unsigned int unique_count;
        bool has_carry_out;
        value_type carry_out;
    } storage;

    const unsigned int flat_id = ::rocprim::flat_block_thread_id();
    const unsigned int batch_id = ::rocprim::detail::block_id<0>();

    unsigned int block_id;
    unsigned int blocks_per_batch;
    if(batch_id < full_batches)
    {
        blocks_per_batch = blocks_per_full_batch;
        block_id = batch_id * blocks_per_batch;
    }
    else
    {
        blocks_per_batch = blocks_per_full_batch - 1;
        block_id = batch_id * blocks_per_batch + full_batches;
    }
    unsigned int block_start = unique_starts[batch_id];

    if(flat_id == 0)
    {
        storage.has_carry_out = (block_id > 0 && blocks_per_batch > 0) &&
            key_compare_op(
                keys_input[block_id * items_per_block - 1],
                keys_input[block_id * items_per_block]
            );
    }

    for(unsigned int bi = 0; bi < blocks_per_batch; bi++)
    {
        const unsigned int block_offset = block_id * items_per_block;

        key_type keys[ItemsPerThread];
        unsigned int valid_count;
        if(block_offset + items_per_block <= size)
        {
            valid_count = items_per_block;
            keys_load_type().load(keys_input + block_offset, keys, storage.keys_load);
        }
        else
        {
            valid_count = size - block_offset;
            keys_load_type().load(keys_input + block_offset, keys, valid_count, storage.keys_load);
        }

        bool head_flags[ItemsPerThread];
        bool tail_flags[ItemsPerThread];
        key_type successor_key = keys[ItemsPerThread - 1];
        ::rocprim::syncthreads();
        if(block_id == blocks - 1)
        {
            discontinuity_type().flag_heads_and_tails(
                head_flags, tail_flags, successor_key, keys,
                guarded_key_flag_op<key_type, KeyCompareFunction>(key_compare_op, valid_count),
                storage.discontinuity
            );
        }
        else
        {
            if(flat_id == BlockSize - 1)
            {
                successor_key = keys_input[block_offset + items_per_block];
            }
            discontinuity_type().flag_heads_and_tails(
                head_flags, tail_flags, successor_key, keys,
                key_flag_op<key_type, KeyCompareFunction>(key_compare_op),
                storage.discontinuity
            );
        }

        value_type values[ItemsPerThread];
        ::rocprim::syncthreads();
        if(block_offset + items_per_block <= size)
        {
            values_load_type().load(values_input + block_offset, values, storage.values_load);
        }
        else
        {
            values_load_type().load(values_input + block_offset, values, valid_count, storage.values_load);
        }

        // Build pairs and run non-commutative inclusive scan to calculate scan-by-key
        // and indices (ranks) of each segment:
        // input:
        //   keys          | 1 1 1 2 3 3 4 4 |
        //   head_flags    | +     + +   +   |
        //   values        | 2 0 1 4 2 3 1 5 |
        // result:
        //   scan values   | 2 2 3 4 2 5 1 6 |
        //   scan keys     | 1 1 1 2 3 3 4 4 |
        //   ranks (key-1) | 0 0 0 1 2 2 3 3 |
        scan_by_key_pair<value_type> pairs[ItemsPerThread];
        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            pairs[i].key = head_flags[i];
            pairs[i].value = values[i];
        }
        scan_by_key_op<scan_by_key_pair<value_type>, BinaryFunction> scan_op(reduce_op);
        ::rocprim::syncthreads();
        scan_type().inclusive_scan(pairs, pairs, storage.scan, scan_op);

        unsigned int ranks[ItemsPerThread];
        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            ranks[i] = pairs[i].key - 1; // The first item is always flagged as head, so indices start from 1
            values[i] = pairs[i].value;
        }

        if(flat_id == BlockSize - 1)
        {
            storage.unique_count = ranks[ItemsPerThread - 1] + (tail_flags[ItemsPerThread - 1] ? 1 : 0);
        }

        if(bi > 0 && storage.has_carry_out)
        {
            // Apply carry-out of the previous block as carry-in for the first segment
            for(unsigned int i = 0; i < ItemsPerThread; i++)
            {
                if(ranks[i] == 0)
                {
                    values[i] = reduce_op(storage.carry_out, values[i]);
                }
            }
        }

        ::rocprim::syncthreads();
        const unsigned int unique_count = storage.unique_count;
        if(flat_id == 0)
        {
            // The first item must be written only if it is the first item of the current segment
            // (otherwise it is written by one of previous blocks)
            head_flags[0] = !storage.has_carry_out;
        }
        if(block_id == blocks - 1)
        {
            // Unflag the head after the last segment as it will be write out of bounds
            for(unsigned int i = 0; i < ItemsPerThread; i++)
            {
                if(ranks[i] >= unique_count)
                {
                    head_flags[i] = false;
                }
            }
        }

        ::rocprim::syncthreads();
        if(flat_id == BlockSize - 1)
        {
            if(bi == blocks_per_batch - 1)
            {
                // Save carry-out of the last block of the current batch
                carry_outs[batch_id].key = keys[ItemsPerThread - 1];
                carry_outs[batch_id].value = values[ItemsPerThread - 1];
                carry_outs[batch_id].destination = block_start + ranks[ItemsPerThread - 1];
                carry_outs[batch_id].is_final_aggregate = tail_flags[ItemsPerThread - 1];
            }
            else
            {
                // Save carry-out to use it as carry-in for the next block of the current batch
                storage.has_carry_out = !tail_flags[ItemsPerThread - 1];
                storage.carry_out = values[ItemsPerThread - 1];
            }
        }

        // Save unique keys and aggregates (some aggregates contains partial values
        // and will be updated later by calculating scan-by-key of carry-outs)
        for(unsigned int i = 0; i < ItemsPerThread; i++)
        {
            if(head_flags[i])
            {
                // Write the key of the first item of the segment as a unique key
                unique_output[block_start + ranks[i]] = keys[i];
            }
            if(tail_flags[i])
            {
                // Write the scanned value of the last item of the segment as an aggregate (reduction of the segment)
                aggregates_output[block_start + ranks[i]] = values[i];
            }
        }

        block_id++;
        block_start += unique_count;
    }
}

template<
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    class AggregatesOutputIterator,
    class CarryOut,
    class KeyCompareFunction,
    class BinaryFunction
>
ROCPRIM_DEVICE inline
void scan_and_scatter_carry_outs(const CarryOut * carry_outs,
                                 AggregatesOutputIterator aggregates_output,
                                 KeyCompareFunction key_compare_op,
                                 BinaryFunction reduce_op,
                                 unsigned int batches)
{
    using key_type = decltype(std::declval<CarryOut>().key);
    using value_type = decltype(std::declval<CarryOut>().value);

    using load_type = ::rocprim::block_load<
        CarryOut, BlockSize, ItemsPerThread,
        ::rocprim::block_load_method::block_load_transpose>;
    using discontinuity_type = ::rocprim::block_discontinuity<key_type, BlockSize>;
    using scan_type = ::rocprim::block_scan<scan_by_key_pair<value_type>, BlockSize>;

    ROCPRIM_SHARED_MEMORY union
    {
        typename load_type::storage_type load;
        typename discontinuity_type::storage_type discontinuity;
        typename scan_type::storage_type scan;
    } storage;

    CarryOut cs[ItemsPerThread];
    load_type().load(carry_outs, cs, batches - 1, storage.load);

    key_type keys[ItemsPerThread];
    value_type values[ItemsPerThread];
    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        keys[i] = cs[i].key;
        values[i] = cs[i].value;
    }

    bool head_flags[ItemsPerThread];
    bool tail_flags[ItemsPerThread];
    const key_type successor_key = keys[ItemsPerThread - 1]; // Do not always flag the last item in the block

    ::rocprim::syncthreads();
    discontinuity_type().flag_heads_and_tails(
        head_flags, tail_flags,
        successor_key, keys,
        guarded_key_flag_op<key_type, KeyCompareFunction>(key_compare_op, batches - 1),
        storage.discontinuity
    );

    scan_by_key_pair<value_type> pairs[ItemsPerThread];
    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        pairs[i].key = head_flags[i];
        pairs[i].value = values[i];
    }

    scan_by_key_op<scan_by_key_pair<value_type>, BinaryFunction> scan_op(reduce_op);
    ::rocprim::syncthreads();
    scan_type().inclusive_scan(pairs, pairs, storage.scan, scan_op);

    // Scatter the last carry-out of each segment as carry-ins
    for(unsigned int i = 0; i < ItemsPerThread; i++)
    {
        if(tail_flags[i])
        {
            const unsigned int dst = cs[i].destination;
            const value_type aggregate = pairs[i].value;
            if(cs[i].is_final_aggregate)
            {
                // Overwrite the aggregate because the next block starts with a different key
                aggregates_output[dst] = aggregate;
            }
            else
            {
                aggregates_output[dst] = reduce_op(aggregate, aggregates_output[dst]);
            }
        }
    }
}

} // end of detail namespace

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_DEVICE_DETAIL_DEVICE_REDUCE_BY_KEY_HPP_
