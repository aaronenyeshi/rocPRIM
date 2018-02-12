// MIT License
//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <algorithm>
#include <functional>
#include <iostream>
#include <type_traits>
#include <vector>
#include <utility>

// Google Test
#include <gtest/gtest.h>

// HC API
#include <hcc/hc.hpp>
// rocPRIM API
#include <rocprim.hpp>

#include "test_utils.hpp"

namespace rp = rocprim;

template<
    class Key,
    class Value,
    bool Descending = false,
    unsigned int StartBit = 0,
    unsigned int EndBit = sizeof(Key) * 8
>
struct params
{
    using key_type = Key;
    using value_type = Value;
    static constexpr bool descending = Descending;
    static constexpr unsigned int start_bit = StartBit;
    static constexpr unsigned int end_bit = EndBit;
};

template<class Params>
class RocprimDeviceRadixSort : public ::testing::Test {
public:
    using params = Params;
};

typedef ::testing::Types<
    params<signed char, double, true>,
    params<int, short>,
    params<short, int, true>,
    params<long long, char>,
    params<double, unsigned int>,
    params<double, int, true>,
    params<float, int>,
    params<float, char, true>,

    // start_bit and end_bit
    params<unsigned char, int, true, 0, 7>,
    params<unsigned short, int, true, 4, 10>,
    params<unsigned int, short, false, 3, 22>,
    params<unsigned int, short, true, 0, 15>,
    params<unsigned long long, char, false, 8, 20>,
    params<unsigned short, double, false, 8, 11>
> Params;

TYPED_TEST_CASE(RocprimDeviceRadixSort, Params);

template<class Key, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_comparator
{
    static_assert(std::is_unsigned<Key>::value, "Test supports start and bits only for unsigned integers");

    bool operator()(const Key& lhs, const Key& rhs)
    {
        auto mask = (1ull << (EndBit - StartBit)) - 1;
        auto l = (static_cast<unsigned long long>(lhs) >> StartBit) & mask;
        auto r = (static_cast<unsigned long long>(rhs) >> StartBit) & mask;
        return Descending ? (r < l) : (l < r);
    }
};

template<class Key, bool Descending>
struct key_comparator<Key, Descending, 0, sizeof(Key) * 8>
{
    bool operator()(const Key& lhs, const Key& rhs)
    {
        return Descending ? (rhs < lhs) : (lhs < rhs);
    }
};

template<class Key, class Value, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_value_comparator
{
    bool operator()(const std::pair<Key, Value>& lhs, const std::pair<Key, Value>& rhs)
    {
        return key_comparator<Key, Descending, StartBit, EndBit>()(lhs.first, rhs.first);
    }
};

std::vector<size_t> get_sizes()
{
    std::vector<size_t> sizes = { 1, 10, 53, 211, 1024, 2345, 4096, 34567, (1 << 16) - 1220, (1 << 23) - 76543 };
    const std::vector<size_t> random_sizes = get_random_data<size_t>(10, 1, 1000000);
    sizes.insert(sizes.end(), random_sizes.begin(), random_sizes.end());
    return sizes;
}

TYPED_TEST(RocprimDeviceRadixSort, SortKeys)
{
    using key_type = typename TestFixture::params::key_type;
    constexpr bool descending = TestFixture::params::descending;
    constexpr unsigned int start_bit = TestFixture::params::start_bit;
    constexpr unsigned int end_bit = TestFixture::params::end_bit;

    hc::accelerator acc;
    hc::accelerator_view acc_view = acc.create_view();

    const bool debug_synchronous = false;

    const std::vector<size_t> sizes = get_sizes();
    for(size_t size : sizes)
    {
        SCOPED_TRACE(testing::Message() << "with size = " << size);

        // Generate data
        std::vector<key_type> keys_input;
        if(std::is_floating_point<key_type>::value)
        {
            keys_input = get_random_data<key_type>(size, (key_type)-1000, (key_type)+1000);
        }
        else
        {
            keys_input = get_random_data<key_type>(
                size,
                std::numeric_limits<key_type>::min(),
                std::numeric_limits<key_type>::max()
            );
        }

        hc::array<key_type> d_keys_input(hc::extent<1>(size), keys_input.begin(), acc_view);
        hc::array<key_type> d_keys_output(size, acc_view);

        // Calculate expected results on host
        std::vector<key_type> expected(keys_input);
        std::stable_sort(expected.begin(), expected.end(), key_comparator<key_type, descending, start_bit, end_bit>());

        size_t temporary_storage_bytes;
        rp::device_radix_sort_keys(
            nullptr, temporary_storage_bytes,
            d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(), size,
            start_bit, end_bit,
            acc_view, debug_synchronous
        );

        ASSERT_GT(temporary_storage_bytes, 0);

        hc::array<char> d_temporary_storage(temporary_storage_bytes, acc_view);

        if(descending)
        {
            rp::device_radix_sort_keys_desc(
                d_temporary_storage.accelerator_pointer(), temporary_storage_bytes,
                d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(), size,
                start_bit, end_bit,
                acc_view, debug_synchronous
            );
        }
        else
        {
            rp::device_radix_sort_keys(
                d_temporary_storage.accelerator_pointer(), temporary_storage_bytes,
                d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(), size,
                start_bit, end_bit,
                acc_view, debug_synchronous
            );
        }
        acc_view.wait();

        std::vector<key_type> keys_output = d_keys_output;
        for(size_t i = 0; i < size; i++)
        {
            ASSERT_EQ(keys_output[i], expected[i]);
        }
    }
}

TYPED_TEST(RocprimDeviceRadixSort, SortKeysValues)
{
    using key_type = typename TestFixture::params::key_type;
    using value_type = typename TestFixture::params::value_type;
    constexpr bool descending = TestFixture::params::descending;
    constexpr unsigned int start_bit = TestFixture::params::start_bit;
    constexpr unsigned int end_bit = TestFixture::params::end_bit;

    hc::accelerator acc;
    hc::accelerator_view acc_view = acc.create_view();

    const bool debug_synchronous = false;

    const std::vector<size_t> sizes = get_sizes();
    for(size_t size : sizes)
    {
        SCOPED_TRACE(testing::Message() << "with size = " << size);

        // Generate data
        std::vector<key_type> keys_input;
        if(std::is_floating_point<key_type>::value)
        {
            keys_input = get_random_data<key_type>(size, (key_type)-1000, (key_type)+1000);
        }
        else
        {
            keys_input = get_random_data<key_type>(
                size,
                std::numeric_limits<key_type>::min(),
                std::numeric_limits<key_type>::max()
            );
        }

        std::vector<value_type> values_input(size);
        std::iota(values_input.begin(), values_input.end(), 0);

        hc::array<key_type> d_keys_input(hc::extent<1>(size), keys_input.begin(), acc_view);
        hc::array<key_type> d_keys_output(size, acc_view);

        hc::array<value_type> d_values_input(hc::extent<1>(size), values_input.begin(), acc_view);
        hc::array<value_type> d_values_output(size, acc_view);

        using key_value = std::pair<key_type, value_type>;

        // Calculate expected results on host
        std::vector<key_value> expected(size);
        for(size_t i = 0; i < size; i++)
        {
            expected[i] = key_value(keys_input[i], values_input[i]);
        }
        std::stable_sort(
            expected.begin(), expected.end(),
            key_value_comparator<key_type, value_type, descending, start_bit, end_bit>()
        );

        size_t temporary_storage_bytes;
        rp::device_radix_sort_pairs(
            nullptr, temporary_storage_bytes,
            d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(),
            d_values_input.accelerator_pointer(), d_values_output.accelerator_pointer(),
            size,
            start_bit, end_bit,
            acc_view, debug_synchronous
        );

        ASSERT_GT(temporary_storage_bytes, 0);

        hc::array<char> d_temporary_storage(temporary_storage_bytes, acc_view);

        if(descending)
        {
            rp::device_radix_sort_pairs_desc(
                d_temporary_storage.accelerator_pointer(), temporary_storage_bytes,
                d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(),
                d_values_input.accelerator_pointer(), d_values_output.accelerator_pointer(),
                size,
                start_bit, end_bit,
                acc_view, debug_synchronous
            );
        }
        else
        {
            rp::device_radix_sort_pairs(
                d_temporary_storage.accelerator_pointer(), temporary_storage_bytes,
                d_keys_input.accelerator_pointer(), d_keys_output.accelerator_pointer(),
                d_values_input.accelerator_pointer(), d_values_output.accelerator_pointer(),
                size,
                start_bit, end_bit,
                acc_view, debug_synchronous
            );
        }
        acc_view.wait();

        std::vector<key_type> keys_output = d_keys_output;
        std::vector<value_type> values_output = d_values_output;

        for(size_t i = 0; i < size; i++)
        {
            ASSERT_EQ(keys_output[i], expected[i].first);
            ASSERT_EQ(values_output[i], expected[i].second);
        }
    }
}