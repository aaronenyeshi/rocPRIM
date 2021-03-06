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

#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>

// Google Test
#include <gtest/gtest.h>
// hipCUB API
#include <hipcub/hipcub.hpp>

#include "test_utils.hpp"

#define HIP_CHECK(error) ASSERT_EQ(error, hipSuccess)

template<
    class T,
    unsigned int WarpSize
>
struct params
{
    using type = T;
    static constexpr unsigned int warp_size = WarpSize;
};

template<class Params>
class HipcubWarpReduceTests : public ::testing::Test {
public:
    using type = typename Params::type;
    static constexpr unsigned int warp_size = Params::warp_size;
};

typedef ::testing::Types<
    // shuffle based reduce
    // Integer
    params<int, 2U>,
    params<int, 4U>,
    params<int, 8U>,
    params<int, 16U>,
    params<int, 32U>,
    #ifdef HIPCUB_ROCPRIM_API
        params<int, 64U>,
    #endif
    // Float
    params<float, 2U>,
    params<float, 4U>,
    params<float, 8U>,
    params<float, 16U>,
    params<float, 32U>,
    #ifdef HIPCUB_ROCPRIM_API
        params<float, 64U>,
    #endif

    // shared memory reduce
    // Integer
    params<int, 3U>,
    params<int, 7U>,
    params<int, 15U>,
    #ifdef HIPCUB_ROCPRIM_API
        params<int, 37U>,
        params<int, 61U>,
    #endif
    // Float
    params<float, 3U>,
    params<float, 7U>,
    params<float, 15U>
    #ifdef HIPCUB_ROCPRIM_API
        ,params<float, 37U>,
        params<float, 61U>
    #endif
> HipcubWarpReduceTestParams;

TYPED_TEST_CASE(HipcubWarpReduceTests, HipcubWarpReduceTestParams);

template<
    class T,
    unsigned int BlockSize,
    unsigned int LogicalWarpSize
>
__global__
void warp_reduce_kernel(T* device_input, T* device_output)
{
    constexpr unsigned int warps_no = BlockSize / LogicalWarpSize;
    const unsigned int warp_id = test_utils::logical_warp_id<LogicalWarpSize>();
    unsigned int index = hipThreadIdx_x + (hipBlockIdx_x * hipBlockDim_x);

    T value = device_input[index];

    using wreduce_t = hipcub::WarpReduce<T, LogicalWarpSize>;
    __shared__ typename wreduce_t::TempStorage storage[warps_no];
    auto reduce_op = hipcub::Sum();
    value = wreduce_t(storage[warp_id]).Reduce(value, reduce_op);

    if (hipThreadIdx_x % LogicalWarpSize == 0)
    {
        device_output[index / LogicalWarpSize] = value;
    }
}

TYPED_TEST(HipcubWarpReduceTests, Reduce)
{
    using T = typename TestFixture::type;
    // logical warp side for warp primitive, execution warp size
    // is always test_utils::warp_size()
    constexpr size_t logical_warp_size = TestFixture::warp_size;
    constexpr size_t block_size =
        test_utils::is_power_of_two(logical_warp_size)
        ? test_utils::max<size_t>(test_utils::warp_size(), logical_warp_size * 4)
        : (test_utils::warp_size()/logical_warp_size) * logical_warp_size;
    unsigned int grid_size = 4;
    const size_t size = block_size * grid_size;

    // Given warp size not supported
    if(logical_warp_size > test_utils::warp_size())
    {
        return;
    }

    // Generate data
    std::vector<T> input = test_utils::get_random_data<T>(size, -100, 100);
    std::vector<T> output(size / logical_warp_size, 0);
    std::vector<T> expected(output.size(), 1);

    // Calculate expected results on host
    for(size_t i = 0; i < output.size(); i++)
    {
        T value = 0;
        for(size_t j = 0; j < logical_warp_size; j++)
        {
            auto idx = i * logical_warp_size + j;
            value += input[idx];
        }
        expected[i] = value;
    }

    // Writing to device memory
    T* device_input;
    HIP_CHECK(hipMalloc(&device_input, input.size() * sizeof(typename decltype(input)::value_type)));
    T* device_output;
    HIP_CHECK(hipMalloc(&device_output, output.size() * sizeof(typename decltype(output)::value_type)));

    HIP_CHECK(
        hipMemcpy(
            device_input, input.data(),
            input.size() * sizeof(T),
            hipMemcpyHostToDevice
        )
    );

    // Launching kernel
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(warp_reduce_kernel<T, block_size, logical_warp_size>),
        dim3(grid_size), dim3(block_size), 0, 0,
        device_input, device_output
    );

    HIP_CHECK(hipPeekAtLastError());
    HIP_CHECK(hipDeviceSynchronize());

    // Read from device memory
    HIP_CHECK(
        hipMemcpy(
            output.data(), device_output,
            output.size() * sizeof(T),
            hipMemcpyDeviceToHost
        )
    );

    for(size_t i = 0; i < output.size(); i++)
    {
        auto diff = std::max<T>(std::abs(0.1f * expected[i]), T(0.01f));
        if(std::is_integral<T>::value) diff = 0;
        ASSERT_NEAR(output[i], expected[i], diff);
    }

    HIP_CHECK(hipFree(device_input));
    HIP_CHECK(hipFree(device_output));
}

template<
    class T,
    unsigned int BlockSize,
    unsigned int LogicalWarpSize
>
__global__
void warp_reduce_valid_kernel(T* device_input, T* device_output, const int valid)
{
    constexpr unsigned int warps_no = BlockSize / LogicalWarpSize;
    const unsigned int warp_id = test_utils::logical_warp_id<LogicalWarpSize>();
    unsigned int index = hipThreadIdx_x + (hipBlockIdx_x * hipBlockDim_x);

    T value = device_input[index];

    using wreduce_t = hipcub::WarpReduce<T, LogicalWarpSize>;
    __shared__ typename wreduce_t::TempStorage storage[warps_no];
    auto reduce_op = hipcub::Sum();
    value = wreduce_t(storage[warp_id]).Reduce(value, reduce_op, valid);

    if (hipThreadIdx_x % LogicalWarpSize == 0)
    {
        device_output[index / LogicalWarpSize] = value;
    }
}

TYPED_TEST(HipcubWarpReduceTests, ReduceValid)
{
    using T = typename TestFixture::type;
    // logical warp side for warp primitive, execution warp size
    // is always test_utils::warp_size()
    constexpr size_t logical_warp_size = TestFixture::warp_size;
    constexpr size_t block_size =
        test_utils::is_power_of_two(logical_warp_size)
        ? test_utils::max<size_t>(test_utils::warp_size(), logical_warp_size * 4)
        : (test_utils::warp_size()/logical_warp_size) * logical_warp_size;
    unsigned int grid_size = 4;
    const size_t size = block_size * grid_size;
    const int valid = logical_warp_size - 1;

    // Given warp size not supported
    if(logical_warp_size > test_utils::warp_size())
    {
        return;
    }

    // Generate data
    std::vector<T> input = test_utils::get_random_data<T>(size, -100, 100);
    std::vector<T> output(size / logical_warp_size, 0);
    std::vector<T> expected(output.size(), 1);

    // Calculate expected results on host
    for(size_t i = 0; i < output.size(); i++)
    {
        T value = 0;
        for(size_t j = 0; j < valid; j++)
        {
            auto idx = i * logical_warp_size + j;
            value += input[idx];
        }
        expected[i] = value;
    }

    // Writing to device memory
    T* device_input;
    HIP_CHECK(hipMalloc(&device_input, input.size() * sizeof(typename decltype(input)::value_type)));
    T* device_output;
    HIP_CHECK(hipMalloc(&device_output, output.size() * sizeof(typename decltype(output)::value_type)));

    HIP_CHECK(
        hipMemcpy(
            device_input, input.data(),
            input.size() * sizeof(T),
            hipMemcpyHostToDevice
        )
    );

    // Launching kernel
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(warp_reduce_valid_kernel<T, block_size, logical_warp_size>),
        dim3(grid_size), dim3(block_size), 0, 0,
        device_input, device_output, valid
    );

    HIP_CHECK(hipPeekAtLastError());
    HIP_CHECK(hipDeviceSynchronize());

    // Read from device memory
    HIP_CHECK(
        hipMemcpy(
            output.data(), device_output,
            output.size() * sizeof(T),
            hipMemcpyDeviceToHost
        )
    );

    for(size_t i = 0; i < output.size(); i++)
    {
        auto diff = std::max<T>(std::abs(0.1f * expected[i]), T(0.01f));
        if(std::is_integral<T>::value) diff = 0;
        ASSERT_NEAR(output[i], expected[i], diff);
    }

    HIP_CHECK(hipFree(device_input));
    HIP_CHECK(hipFree(device_output));
}
