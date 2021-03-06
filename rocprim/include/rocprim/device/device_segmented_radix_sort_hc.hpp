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

#ifndef ROCPRIM_DEVICE_DEVICE_SEGMENTED_RADIX_SORT_HC_HPP_
#define ROCPRIM_DEVICE_DEVICE_SEGMENTED_RADIX_SORT_HC_HPP_

#include <iostream>
#include <iterator>
#include <type_traits>
#include <utility>

#include "../config.hpp"
#include "../detail/various.hpp"
#include "../detail/radix_sort.hpp"

#include "../intrinsics.hpp"
#include "../functional.hpp"
#include "../types.hpp"

#include "detail/device_segmented_radix_sort.hpp"

/// \addtogroup devicemodule_hc
/// @{

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

#define ROCPRIM_DETAIL_HC_SYNC(name, size, start) \
    { \
        if(debug_synchronous) \
        { \
            std::cout << name << "(" << size << ")"; \
            acc_view.wait(); \
            auto end = std::chrono::high_resolution_clock::now(); \
            auto d = std::chrono::duration_cast<std::chrono::duration<double>>(end - start); \
            std::cout << " " << d.count() * 1000 << " ms" << '\n'; \
        } \
    }

template<
    bool Descending,
    class KeysInputIterator,
    class KeysOutputIterator,
    class ValuesInputIterator,
    class ValuesOutputIterator,
    class OffsetIterator
>
inline
void segmented_radix_sort_impl(void * temporary_storage,
                               size_t& storage_size,
                               KeysInputIterator keys_input,
                               typename std::iterator_traits<KeysInputIterator>::value_type * keys_tmp,
                               KeysOutputIterator keys_output,
                               ValuesInputIterator values_input,
                               typename std::iterator_traits<ValuesInputIterator>::value_type * values_tmp,
                               ValuesOutputIterator values_output,
                               unsigned int size,
                               bool& is_result_in_output,
                               unsigned int segments,
                               OffsetIterator begin_offsets,
                               OffsetIterator end_offsets,
                               unsigned int begin_bit,
                               unsigned int end_bit,
                               hc::accelerator_view& acc_view,
                               bool debug_synchronous)
{
    constexpr unsigned int radix_bits = 8;

    using key_type = typename std::iterator_traits<KeysInputIterator>::value_type;
    using value_type = typename std::iterator_traits<ValuesInputIterator>::value_type;

    constexpr bool with_values = !std::is_same<value_type, ::rocprim::empty_type>::value;

    constexpr unsigned int block_size = 256;
    constexpr unsigned int items_per_thread = 11;

    const unsigned int iterations = ::rocprim::detail::ceiling_div(end_bit - begin_bit, radix_bits);
    const bool with_double_buffer = keys_tmp != nullptr;

    const size_t keys_bytes = ::rocprim::detail::align_size(size * sizeof(key_type));
    const size_t values_bytes = with_values ? ::rocprim::detail::align_size(size * sizeof(value_type)) : 0;
    if(temporary_storage == nullptr)
    {
        if(!with_double_buffer)
        {
            storage_size = keys_bytes + values_bytes;
        }
        else
        {
            storage_size = 4;
        }
        return;
    }

    if(debug_synchronous)
    {
        std::cout << "iterations " << iterations << '\n';
        acc_view.wait();
    }

    char * ptr = reinterpret_cast<char *>(temporary_storage);
    if(!with_double_buffer)
    {
        keys_tmp = reinterpret_cast<key_type *>(ptr);
        ptr += keys_bytes;
        values_tmp = with_values ? reinterpret_cast<value_type *>(ptr) : nullptr;
    }

    bool to_output = with_double_buffer || (iterations - 1) % 2 == 0;
    for(unsigned int bit = begin_bit; bit < end_bit; bit += radix_bits)
    {
        // Handle cases when (end_bit - bit) is not divisible by radix_bits, i.e. the last
        // iteration has a shorter mask.
        const unsigned int current_radix_bits = ::rocprim::min(radix_bits, end_bit - bit);

        const bool is_first_iteration = (bit == begin_bit);

        std::chrono::high_resolution_clock::time_point start;

        if(debug_synchronous) start = std::chrono::high_resolution_clock::now();
        if(is_first_iteration)
        {
            if(to_output)
            {
                hc::parallel_for_each(
                    acc_view,
                    hc::tiled_extent<1>(segments * block_size, block_size),
                    [=](hc::tiled_index<1>) [[hc]]
                    {
                        segmented_sort<block_size, items_per_thread, radix_bits, Descending>(
                            keys_input, keys_output, values_input, values_output,
                            begin_offsets, end_offsets,
                            bit, current_radix_bits
                        );
                    }
                );
            }
            else
            {
                hc::parallel_for_each(
                    acc_view,
                    hc::tiled_extent<1>(segments * block_size, block_size),
                    [=](hc::tiled_index<1>) [[hc]]
                    {
                        segmented_sort<block_size, items_per_thread, radix_bits, Descending>(
                            keys_input, keys_tmp, values_input, values_tmp,
                            begin_offsets, end_offsets,
                            bit, current_radix_bits
                        );
                    }
                );
            }
        }
        else
        {
            if(to_output)
            {
                hc::parallel_for_each(
                    acc_view,
                    hc::tiled_extent<1>(segments * block_size, block_size),
                    [=](hc::tiled_index<1>) [[hc]]
                    {
                        segmented_sort<block_size, items_per_thread, radix_bits, Descending>(
                            keys_tmp, keys_output, values_tmp, values_output,
                            begin_offsets, end_offsets,
                            bit, current_radix_bits
                        );
                    }
                );
            }
            else
            {
                hc::parallel_for_each(
                    acc_view,
                    hc::tiled_extent<1>(segments * block_size, block_size),
                    [=](hc::tiled_index<1>) [[hc]]
                    {
                        segmented_sort<block_size, items_per_thread, radix_bits, Descending>(
                            keys_output, keys_tmp, values_output, values_tmp,
                            begin_offsets, end_offsets,
                            bit, current_radix_bits
                        );
                    }
                );
            }
        }
        ROCPRIM_DETAIL_HC_SYNC("segmented_sort", segments, start);

        is_result_in_output = to_output;
        to_output = !to_output;
    }
}

#undef ROCPRIM_DETAIL_HC_SYNC

} // end namespace detail

/// \brief HC parallel ascending radix sort primitive for device level.
///
/// \p segmented_radix_sort_keys function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of keys. Function sorts input keys in ascending order.
///
/// \par Overview
/// * The contents of the inputs are not altered by the sorting function.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * \p Key type (a \p value_type of \p KeysInputIterator and \p KeysOutputIterator) must be
/// an arithmetic type (that is, an integral type or a floating-point type).
/// * Ranges specified by \p keys_input and \p keys_output must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam KeysInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam KeysOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] keys_input - pointer to the first element in the range to sort.
/// \param [out] keys_output - pointer to the first element in the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level ascending radix sort is performed on an array of
/// \p float values.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// size_t input_size;        // e.g., 8
/// hc::array<float> input;   // e.g., [0.6, 0.3, 0.65, 0.4, 0.2, 0.08, 1, 0.7]
/// hc::array<float> output;  // empty array of 8 elements
/// unsigned int segments;    // e.g., 3
/// hc::array<int> offsets;   // e.g. [0, 2, 3, 8]
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_keys(
///     nullptr, temporary_storage_size_bytes,
///     input.accelerator_pointer(), output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(float), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_keys(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     input.accelerator_pointer(), output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(float), acc_view
/// );
/// // keys_output: [0.3, 0.6, 0.65, 0.08, 0.2, 0.4, 0.7, 1]
/// \endcode
/// \endparblock
template<
    class KeysInputIterator,
    class KeysOutputIterator,
    class OffsetIterator,
    class Key = typename std::iterator_traits<KeysInputIterator>::value_type
>
inline
void segmented_radix_sort_keys(void * temporary_storage,
                               size_t& storage_size,
                               KeysInputIterator keys_input,
                               KeysOutputIterator keys_output,
                               unsigned int size,
                               unsigned int segments,
                               OffsetIterator begin_offsets,
                               OffsetIterator end_offsets,
                               unsigned int begin_bit = 0,
                               unsigned int end_bit = 8 * sizeof(Key),
                               hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                               bool debug_synchronous = false)
{
    empty_type * values = nullptr;
    bool ignored;
    detail::segmented_radix_sort_impl<false>(
        temporary_storage, storage_size,
        keys_input, nullptr, keys_output,
        values, nullptr, values,
        size, ignored,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
}

/// \brief HC parallel descending radix sort primitive for device level.
///
/// \p segmented_radix_sort_keys_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of keys. Function sorts input keys in descending order.
///
/// \par Overview
/// * The contents of the inputs are not altered by the sorting function.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * \p Key type (a \p value_type of \p KeysInputIterator and \p KeysOutputIterator) must be
/// an arithmetic type (that is, an integral type or a floating-point type).
/// * Ranges specified by \p keys_input and \p keys_output must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam KeysInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam KeysOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] keys_input - pointer to the first element in the range to sort.
/// \param [out] keys_output - pointer to the first element in the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level descending radix sort is performed on an array of
/// integer values.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// size_t input_size;        // e.g., 8
/// hc::array<int> input;     // e.g., [6, 3, 5, 4, 2, 8, 1, 7]
/// hc::array<int> output;    // empty array of 8 elements
/// unsigned int segments;    // e.g., 3
/// hc::array<int> offsets;   // e.g. [0, 2, 3, 8]
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_keys_desc(
///     nullptr, temporary_storage_size_bytes,
///     input.accelerator_pointer(), output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_keys_desc(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     input.accelerator_pointer(), output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
/// // keys_output: [6, 3, 5, 8, 7, 4, 2, 1]
/// \endcode
/// \endparblock
template<
    class KeysInputIterator,
    class KeysOutputIterator,
    class OffsetIterator,
    class Key = typename std::iterator_traits<KeysInputIterator>::value_type
>
inline
void segmented_radix_sort_keys_desc(void * temporary_storage,
                                    size_t& storage_size,
                                    KeysInputIterator keys_input,
                                    KeysOutputIterator keys_output,
                                    unsigned int size,
                                    unsigned int segments,
                                    OffsetIterator begin_offsets,
                                    OffsetIterator end_offsets,
                                    unsigned int begin_bit = 0,
                                    unsigned int end_bit = 8 * sizeof(Key),
                                    hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                    bool debug_synchronous = false)
{
    empty_type * values = nullptr;
    bool ignored;
    detail::segmented_radix_sort_impl<true>(
        temporary_storage, storage_size,
        keys_input, nullptr, keys_output,
        values, nullptr, values,
        size, ignored,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
}

/// \brief HC parallel ascending radix sort-by-key primitive for device level.
///
/// \p segmented_radix_sort_pairs_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of (key, value) pairs. Function sorts input pairs in ascending order of keys.
///
/// \par Overview
/// * The contents of the inputs are not altered by the sorting function.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * \p Key type (a \p value_type of \p KeysInputIterator and \p KeysOutputIterator) must be
/// an arithmetic type (that is, an integral type or a floating-point type).
/// * Ranges specified by \p keys_input, \p keys_output, \p values_input and \p values_output must
/// have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam KeysInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam KeysOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam ValuesInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam ValuesOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] keys_input - pointer to the first element in the range to sort.
/// \param [out] keys_output - pointer to the first element in the output range.
/// \param [in] values_input - pointer to the first element in the range to sort.
/// \param [out] values_output - pointer to the first element in the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level ascending radix sort is performed where input keys are
/// represented by an array of unsigned integers and input values by an array of <tt>double</tt>s.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// size_t input_size;          // e.g., 8
/// hc::array<unsigned int> keys_input;  // e.g., [ 6, 3,  5, 4,  1,  8,  1, 7]
/// hc::array<double> values_input;      // e.g., [-5, 2, -4, 3, -1, -8, -2, 7]
/// hc::array<unsigned int> keys_output; // empty array of 8 elements
/// hc::array<double> values_output;     // empty array of 8 elements
/// unsigned int segments;               // e.g., 3
/// hc::array<int> offsets;              // e.g. [0, 2, 3, 8]
///
/// // Keys are in range [0; 8], so we can limit compared bit to bits on indexes
/// // 0, 1, 2, 3, and 4. In order to do this begin_bit is set to 0 and end_bit
/// // is set to 5.
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_pairs(
///     nullptr, temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), keys_output.accelerator_pointer(),
///     values_input.accelerator_pointer(), values_output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 5, acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_pairs(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), keys_output.accelerator_pointer(),
///     values_input.accelerator_pointer(), values_output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 5, acc_view
/// );
/// // keys_output:   [3,  6,  5,  1,  1, 4, 7,  8]
/// // values_output: [2, -5, -4, -1, -2, 3, 7, -8]
/// \endcode
/// \endparblock
template<
    class KeysInputIterator,
    class KeysOutputIterator,
    class ValuesInputIterator,
    class ValuesOutputIterator,
    class OffsetIterator,
    class Key = typename std::iterator_traits<KeysInputIterator>::value_type
>
inline
void segmented_radix_sort_pairs(void * temporary_storage,
                                size_t& storage_size,
                                KeysInputIterator keys_input,
                                KeysOutputIterator keys_output,
                                ValuesInputIterator values_input,
                                ValuesOutputIterator values_output,
                                unsigned int size,
                                unsigned int segments,
                                OffsetIterator begin_offsets,
                                OffsetIterator end_offsets,
                                unsigned int begin_bit = 0,
                                unsigned int end_bit = 8 * sizeof(Key),
                                hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                bool debug_synchronous = false)
{
    bool ignored;
    detail::segmented_radix_sort_impl<false>(
        temporary_storage, storage_size,
        keys_input, nullptr, keys_output,
        values_input, nullptr, values_output,
        size, ignored,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
}

/// \brief HC parallel descending radix sort-by-key primitive for device level.
///
/// \p segmented_radix_sort_pairs_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of (key, value) pairs. Function sorts input pairs in descending order of keys.
///
/// \par Overview
/// * The contents of the inputs are not altered by the sorting function.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * \p Key type (a \p value_type of \p KeysInputIterator and \p KeysOutputIterator) must be
/// an arithmetic type (that is, an integral type or a floating-point type).
/// * Ranges specified by \p keys_input, \p keys_output, \p values_input and \p values_output must
/// have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam KeysInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam KeysOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam ValuesInputIterator - random-access iterator type of the input range. Must meet the
/// requirements of a C++ InputIterator concept. It can be a simple pointer type.
/// \tparam ValuesOutputIterator - random-access iterator type of the output range. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in] keys_input - pointer to the first element in the range to sort.
/// \param [out] keys_output - pointer to the first element in the output range.
/// \param [in] values_input - pointer to the first element in the range to sort.
/// \param [out] values_output - pointer to the first element in the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level descending radix sort is performed where input keys are
/// represented by an array of integers and input values by an array of <tt>double</tt>s.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and output (declare pointers, allocate device memory etc.)
/// size_t input_size;                // e.g., 8
/// hc::array<int> keys_input;        // e.g., [ 6, 3,  5, 4,  1,  8,  1, 7]
/// hc::array<double> values_input;   // e.g., [-5, 2, -4, 3, -1, -8, -2, 7]
/// hc::array<int> keys_output;       // empty array of 8 elements
/// hc::array<double> values_output;  // empty array of 8 elements
/// unsigned int segments;            // e.g., 3
/// hc::array<int> offsets;           // e.g. [0, 2, 3, 8]
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_pairs_desc(
///     nullptr, temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), keys_output.accelerator_pointer(),
///     values_input.accelerator_pointer(), values_output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_pairs_desc(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys_input.accelerator_pointer(), keys_output.accelerator_pointer(),
///     values_input.accelerator_pointer(), values_output.accelerator_pointer(),
///     input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
/// // keys_output:   [ 6, 3,  5,  8, 7, 4,  1,  1]
/// // values_output: [-5, 2, -4, -8, 7, 3, -1, -2]
/// \endcode
/// \endparblock
template<
    class KeysInputIterator,
    class KeysOutputIterator,
    class ValuesInputIterator,
    class ValuesOutputIterator,
    class OffsetIterator,
    class Key = typename std::iterator_traits<KeysInputIterator>::value_type
>
inline
void segmented_radix_sort_pairs_desc(void * temporary_storage,
                                     size_t& storage_size,
                                     KeysInputIterator keys_input,
                                     KeysOutputIterator keys_output,
                                     ValuesInputIterator values_input,
                                     ValuesOutputIterator values_output,
                                     unsigned int size,
                                     unsigned int segments,
                                     OffsetIterator begin_offsets,
                                     OffsetIterator end_offsets,
                                     unsigned int begin_bit = 0,
                                     unsigned int end_bit = 8 * sizeof(Key),
                                     hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                     bool debug_synchronous = false)
{
    bool ignored;
    detail::segmented_radix_sort_impl<true>(
        temporary_storage, storage_size,
        keys_input, nullptr, keys_output,
        values_input, nullptr, values_output,
        size, ignored,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
}

/// \brief HC parallel ascending radix sort primitive for device level.
///
/// \p segmented_radix_sort_keys function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of keys. Function sorts input keys in ascending order.
///
/// \par Overview
/// * The contents of both buffers of \p keys may be altered by the sorting function.
/// * \p current() of \p keys is used as the input.
/// * The function will update \p current() of \p keys to point to the buffer
/// that contains the output range.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * The function requires small \p temporary_storage as it does not need
/// a temporary buffer of \p size elements.
/// * \p Key type must be an arithmetic type (that is, an integral type or a floating-point
/// type).
/// * Buffers of \p keys must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam Key - key type. Must be an integral type or a floating-point type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in,out] keys - reference to the double-buffer of keys, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level ascending radix sort is performed on an array of
/// \p float values.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and tmp (declare pointers, allocate device memory etc.)
/// size_t input_size;        // e.g., 8
/// hc::array<float> input;   // e.g., [0.6, 0.3, 0.65, 0.4, 0.2, 0.08, 1, 0.7]
/// hc::array<float> tmp;     // empty array of 8 elements
/// unsigned int segments;    // e.g., 3
/// hc::array<int> offsets;   // e.g. [0, 2, 3, 8]
/// // Create double-buffer
/// rocprim::double_buffer<float> keys(input.accelerator_pointer(), tmp.accelerator_pointer());
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_keys(
///     nullptr, temporary_storage_size_bytes,
///     keys, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(float), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_keys(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(float), acc_view
/// );
/// // keys.current(): [0.3, 0.6, 0.65, 0.08, 0.2, 0.4, 0.7, 1]
/// \endcode
/// \endparblock
template<class Key, class OffsetIterator>
inline
void segmented_radix_sort_keys(void * temporary_storage,
                               size_t& storage_size,
                               double_buffer<Key>& keys,
                               unsigned int size,
                               unsigned int segments,
                               OffsetIterator begin_offsets,
                               OffsetIterator end_offsets,
                               unsigned int begin_bit = 0,
                               unsigned int end_bit = 8 * sizeof(Key),
                               hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                               bool debug_synchronous = false)
{
    empty_type * values = nullptr;
    bool is_result_in_output;
    detail::segmented_radix_sort_impl<false>(
        temporary_storage, storage_size,
        keys.current(), keys.current(), keys.alternate(),
        values, values, values,
        size, is_result_in_output,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
    if(temporary_storage != nullptr && is_result_in_output)
    {
        keys.swap();
    }
}

/// \brief HC parallel descending radix sort primitive for device level.
///
/// \p segmented_radix_sort_keys_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of keys. Function sorts input keys in descending order.
///
/// \par Overview
/// * The contents of both buffers of \p keys may be altered by the sorting function.
/// * \p current() of \p keys is used as the input.
/// * The function will update \p current() of \p keys to point to the buffer
/// that contains the output range.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * The function requires small \p temporary_storage as it does not need
/// a temporary buffer of \p size elements.
/// * \p Key type must be an arithmetic type (that is, an integral type or a floating-point
/// type).
/// * Buffers of \p keys must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam Key - key type. Must be an integral type or a floating-point type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in,out] keys - reference to the double-buffer of keys, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level descending radix sort is performed on an array of
/// integer values.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and tmp (declare pointers, allocate device memory etc.)
/// size_t input_size;        // e.g., 8
/// hc::array<int> input;     // e.g., [6, 3, 5, 4, 2, 8, 1, 7]
/// hc::array<int> tmp;       // empty array of 8 elements
/// unsigned int segments;    // e.g., 3
/// hc::array<int> offsets;   // e.g. [0, 2, 3, 8]
/// // Create double-buffer
/// rocprim::double_buffer<int> keys(input.accelerator_pointer(), tmp.accelerator_pointer());
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_keys_desc(
///     nullptr, temporary_storage_size_bytes,
///     keys, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_keys_desc(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
/// // keys.current(): [6, 3, 5, 8, 7, 4, 2, 1]
/// \endcode
/// \endparblock
template<class Key, class OffsetIterator>
inline
void segmented_radix_sort_keys_desc(void * temporary_storage,
                                    size_t& storage_size,
                                    double_buffer<Key>& keys,
                                    unsigned int size,
                                    unsigned int segments,
                                    OffsetIterator begin_offsets,
                                    OffsetIterator end_offsets,
                                    unsigned int begin_bit = 0,
                                    unsigned int end_bit = 8 * sizeof(Key),
                                    hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                    bool debug_synchronous = false)
{
    empty_type * values = nullptr;
    bool is_result_in_output;
    detail::segmented_radix_sort_impl<true>(
        temporary_storage, storage_size,
        keys.current(), keys.current(), keys.alternate(),
        values, values, values,
        size, is_result_in_output,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
    if(temporary_storage != nullptr && is_result_in_output)
    {
        keys.swap();
    }
}

/// \brief HC parallel ascending radix sort-by-key primitive for device level.
///
/// \p segmented_radix_sort_pairs_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of (key, value) pairs. Function sorts input pairs in ascending order of keys.
///
/// \par Overview
/// * The contents of both buffers of \p keys and \p values may be altered by the sorting function.
/// * \p current() of \p keys and \p values are used as the input.
/// * The function will update \p current() of \p keys and \p values to point to buffers
/// that contains the output range.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * The function requires small \p temporary_storage as it does not need
/// a temporary buffer of \p size elements.
/// * \p Key type must be an arithmetic type (that is, an integral type or a floating-point
/// type).
/// * Buffers of \p keys must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam Key - key type. Must be an integral type or a floating-point type.
/// \tparam Value - value type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in,out] keys - reference to the double-buffer of keys, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in,out] values - reference to the double-buffer of values, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level ascending radix sort is performed where input keys are
/// represented by an array of unsigned integers and input values by an array of <tt>double</tt>s.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and tmp (declare pointers, allocate device memory etc.)
/// size_t input_size;                   // e.g., 8
/// hc::array<unsigned int> keys_input;  // e.g., [ 6, 3,  5, 4,  1,  8,  1, 7]
/// hc::array<double> values_input;      // e.g., [-5, 2, -4, 3, -1, -8, -2, 7]
/// hc::array<unsigned int> keys_tmp;    // empty array of 8 elements
/// hc::array<double> values_tmp;        // empty array of 8 elements
/// unsigned int segments;               // e.g., 3
/// hc::array<int> offsets;              // e.g. [0, 2, 3, 8]
/// // Create double-buffers
/// rocprim::double_buffer<unsigned int> keys(keys_input.accelerator_pointer(), keys_tmp.accelerator_pointer());
/// rocprim::double_buffer<double> values(values_input.accelerator_pointer(), values_tmp.accelerator_pointer());
///
/// // Keys are in range [0; 8], so we can limit compared bit to bits on indexes
/// // 0, 1, 2, 3, and 4. In order to do this begin_bit is set to 0 and end_bit
/// // is set to 5.
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_pairs(
///     nullptr, temporary_storage_size_bytes,
///     keys, values, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1
///     0, 5, acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_pairs(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys, values, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1
///     0, 5, acc_view
/// );
/// // keys.current():   [3,  6,  5,  1,  1, 4, 7,  8]
/// // values.current(): [2, -5, -4, -1, -2, 3, 7, -8]
/// \endcode
/// \endparblock
template<class Key, class Value, class OffsetIterator>
inline
void segmented_radix_sort_pairs(void * temporary_storage,
                                size_t& storage_size,
                                double_buffer<Key>& keys,
                                double_buffer<Value>& values,
                                unsigned int size,
                                unsigned int segments,
                                OffsetIterator begin_offsets,
                                OffsetIterator end_offsets,
                                unsigned int begin_bit = 0,
                                unsigned int end_bit = 8 * sizeof(Key),
                                hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                bool debug_synchronous = false)
{
    bool is_result_in_output;
    detail::segmented_radix_sort_impl<false>(
        temporary_storage, storage_size,
        keys.current(), keys.current(), keys.alternate(),
        values.current(), values.current(), values.alternate(),
        size, is_result_in_output,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
    if(temporary_storage != nullptr && is_result_in_output)
    {
        keys.swap();
        values.swap();
    }
}

/// \brief HC parallel descending radix sort-by-key primitive for device level.
///
/// \p segmented_radix_sort_pairs_desc function performs a device-wide radix sort across multiple,
/// non-overlapping sequences of (key, value) pairs. Function sorts input pairs in descending order of keys.
///
/// \par Overview
/// * The contents of both buffers of \p keys and \p values may be altered by the sorting function.
/// * \p current() of \p keys and \p values are used as the input.
/// * The function will update \p current() of \p keys and \p values to point to buffers
/// that contains the output range.
/// * Returns the required size of \p temporary_storage in \p storage_size
/// if \p temporary_storage in a null pointer.
/// * The function requires small \p temporary_storage as it does not need
/// a temporary buffer of \p size elements.
/// * \p Key type must be an arithmetic type (that is, an integral type or a floating-point
/// type).
/// * Buffers of \p keys must have at least \p size elements.
/// * Ranges specified by \p begin_offsets and \p end_offsets must have
/// at least \p segments elements. They may use the same sequence <tt>offsets</tt> of at least
/// <tt>segments + 1</tt> elements: <tt>offsets</tt> for \p begin_offsets and
/// <tt>offsets + 1</tt> for \p end_offsets.
/// * If \p Key is an integer type and the range of keys is known in advance, the performance
/// can be improved by setting \p begin_bit and \p end_bit, for example if all keys are in range
/// [100, 10000], <tt>begin_bit = 0</tt> and <tt>end_bit = 14</tt> will cover the whole range.
///
/// \tparam Key - key type. Must be an integral type or a floating-point type.
/// \tparam Value - value type.
/// \tparam OffsetIterator - random-access iterator type of segment offsets. Must meet the
/// requirements of a C++ OutputIterator concept. It can be a simple pointer type.
///
/// \param [in] temporary_storage - pointer to a device-accessible temporary storage. When
/// a null pointer is passed, the required allocation size (in bytes) is written to
/// \p storage_size and function returns without performing the sort operation.
/// \param [in,out] storage_size - reference to a size (in bytes) of \p temporary_storage.
/// \param [in,out] keys - reference to the double-buffer of keys, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in,out] values - reference to the double-buffer of values, its \p current()
/// contains the input range and will be updated to point to the output range.
/// \param [in] size - number of element in the input range.
/// \param [in] segments - number of segments in the input range.
/// \param [in] begin_offsets - iterator to the first element in the range of beginning offsets.
/// \param [in] end_offsets - iterator to the first element in the range of ending offsets.
/// \param [in] begin_bit - [optional] index of the first (least significant) bit used in
/// key comparison. Must be in range <tt>[0; 8 * sizeof(Key))</tt>. Default value: \p 0.
/// \param [in] end_bit - [optional] past-the-end index (most significant) bit used in
/// key comparison. Must be in range <tt>(begin_bit; 8 * sizeof(Key)]</tt>. Default
/// value: \p <tt>8 * sizeof(Key)</tt>.
/// \param [in] acc_view - [optional] \p hc::accelerator_view object. The default value
/// is \p hc::accelerator().get_default_view() (default view of the default accelerator).
/// \param [in] debug_synchronous - [optional] If true, synchronization after every kernel
/// launch is forced in order to check for errors. Default value is \p false.
///
/// \par Example
/// \parblock
/// In this example a device-level descending radix sort is performed where input keys are
/// represented by an array of integers and input values by an array of <tt>double</tt>s.
///
/// \code{.cpp}
/// #include <rocprim/rocprim.hpp>
///
/// hc::accelerator_view acc_view = ...;
///
/// // Prepare input and tmp (declare pointers, allocate device memory etc.)
/// size_t input_size;                // e.g., 8
/// hc::array<int> keys_input;        // e.g., [ 6, 3,  5, 4,  1,  8,  1, 7]
/// hc::array<double> values_input;   // e.g., [-5, 2, -4, 3, -1, -8, -2, 7]
/// hc::array<int> keys_tmp;          // empty array of 8 elements
/// hc::array<double> values_tmp;     // empty array of 8 elements
/// unsigned int segments;            // e.g., 3
/// hc::array<int> offsets;           // e.g. [0, 2, 3, 8]
/// // Create double-buffers
/// rocprim::double_buffer<int> keys(keys_input.accelerator_pointer(), keys_tmp.accelerator_pointer());
/// rocprim::double_buffer<double> values(values_input.accelerator_pointer(), values_tmp.accelerator_pointer());
///
/// size_t temporary_storage_size_bytes;
/// // Get required size of the temporary storage
/// rocprim::segmented_radix_sort_pairs_desc(
///     nullptr, temporary_storage_size_bytes,
///     keys, values, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
///
/// // allocate temporary storage
/// hc::array<char> temporary_storage(temporary_storage_size_bytes, acc_view);
///
/// // perform sort
/// rocprim::segmented_radix_sort_pairs_desc(
///     temporary_storage.accelerator_pointer(), temporary_storage_size_bytes,
///     keys, values, input_size,
///     segments, offsets.accelerator_pointer(), offsets.accelerator_pointer() + 1,
///     0, 8 * sizeof(int), acc_view
/// );
/// // keys.current():   [ 6, 3,  5,  8, 7, 4,  1,  1]
/// // values.current(): [-5, 2, -4, -8, 7, 3, -1, -2]
/// \endcode
/// \endparblock
template<class Key, class Value, class OffsetIterator>
inline
void segmented_radix_sort_pairs_desc(void * temporary_storage,
                                     size_t& storage_size,
                                     double_buffer<Key>& keys,
                                     double_buffer<Value>& values,
                                     unsigned int size,
                                     unsigned int segments,
                                     OffsetIterator begin_offsets,
                                     OffsetIterator end_offsets,
                                     unsigned int begin_bit = 0,
                                     unsigned int end_bit = 8 * sizeof(Key),
                                     hc::accelerator_view acc_view = hc::accelerator().get_default_view(),
                                     bool debug_synchronous = false)
{
    bool is_result_in_output;
    detail::segmented_radix_sort_impl<true>(
        temporary_storage, storage_size,
        keys.current(), keys.current(), keys.alternate(),
        values.current(), values.current(), values.alternate(),
        size, is_result_in_output,
        segments, begin_offsets, end_offsets,
        begin_bit, end_bit,
        acc_view, debug_synchronous
    );
    if(temporary_storage != nullptr && is_result_in_output)
    {
        keys.swap();
        values.swap();
    }
}

END_ROCPRIM_NAMESPACE

/// @}
// end of group devicemodule_hc

#endif // ROCPRIM_DEVICE_DEVICE_SEGMENTED_RADIX_SORT_HC_HPP_
