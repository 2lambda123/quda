#pragma once

#include <target_device.h>
#include <reducer.h>
#include <group_reduce.h>
#include <special_ops.h>

/**
   @file block_reduce_helper.h

   @section This files contains the CUDA device specializations for
   warp- and block-level reductions, using the CUB library
 */

//using namespace quda;

namespace quda
{

  /**
     @brief The atomic word size we use for a given reduction type.
     This type should be lock-free to guarantee correct behaviour on
     platforms that are not coherent with respect to the host
   */
  template <typename T, typename Enable = void> struct atomic_type;

  template <> struct atomic_type<device_reduce_t> {
    using type = device_reduce_t;
  };

  template <> struct atomic_type<float> {
    using type = float;
  };

  template <typename T> struct atomic_type<T, std::enable_if_t<std::is_same_v<T, array<device_reduce_t, T::N>>>> {
    using type = device_reduce_t;
  };

  template <typename T>
  struct atomic_type<T, std::enable_if_t<std::is_same_v<T, array<array<device_reduce_t, T::value_type::N>, T::N>>>> {
    using type = device_reduce_t;
  };

  template <typename T> struct atomic_type<T, std::enable_if_t<std::is_same_v<T, array<complex<double>, T::N>>>> {
    using type = double;
  };

  template <typename T> struct atomic_type<T, std::enable_if_t<std::is_same_v<T, array<complex<float>, T::N>>>> {
    using type = float;
  };

  // pre-declaration of warp_reduce that we wish to specialize
  template <bool> struct warp_reduce;

  /**
     @brief SYCL specialization of warp_reduce, utilizing subgroup operations
  */
  template <> struct warp_reduce<true> {

    /**
       @brief Perform a warp-wide reduction using subgroups
       @param[in] value_ thread-local value to be reduced
       @param[in] all Whether we want all threads to have visibility
       to the result (all = true) or just the first thread in the
       warp (all = false).
       @param[in] r The reduction operation we want to apply
       @return The warp-wide reduced value
     */
    template <typename T, typename reducer_t, typename param_t>
    T inline operator()(const T &value_, bool all, const reducer_t &r, const param_t &)
    {
      auto sg = sycl::ext::oneapi::experimental::this_sub_group();
      T value = value_;
#pragma unroll
      for (int offset = param_t::width/2; offset >= 1; offset /= 2) {
	value = r(value, sycl::shift_group_left(sg, value, offset));
      }
      //if (all) value = sycl::select_from_group(sg, value, 0);
      if (all) value = sycl::group_broadcast(sg, value);
      return value;
    }

  };


  // pre-declaration of block_reduce that we wish to specialize
  template <bool> struct block_reduce;

  /**
     @brief SYCL specialization of block_reduce, using SYCL group reductions
  */
  struct block_reduceG {
    /**
       @brief Perform a block-wide reduction
       @param[in] value_ thread-local value to be reduced
       @param[in] async Whether this reduction will be performed
       asynchronously with respect to the calling threads
       @param[in] batch The batch index of the reduction
       @param[in] all Whether we want all threads to have visibility
       to the result (all = true) or just the first thread in the
       block (all = false)
       @param[in] r The reduction operation we want to apply
       @return The block-wide reduced value
     */
    template <typename T, typename reducer_t, typename param_t, typename ...B>
    inline T operator()(const T &value_, bool async, int batch, bool all,
			const reducer_t &r, const param_t &, B *...opBlockReduce)
    {
      if (!async) __syncthreads(); // only synchronize if we are not pipelining
      const int nbatch = param_t::batch_size;
      //const int nbatch = std::min(param_t::batch_size, localRangeZ);
      auto grp = getGroup();
      T result;
      //T result = reducer_t::init();
      for(int i=0; i<nbatch; i++) {
	T in = (i==batch) ? value_ : reducer_t::init();
	T out;
	blockReduce(grp, out, in, r);
	if(i==batch) result = out;
      }
      return result;
    }
  };

  /**
     @brief SYCL specialization of block_reduce, building on the warp_reduce
  */
  struct block_reduceW {

    template <int width_> struct warp_reduce_param {
      static constexpr int width = width_;
    };

    /**
       @brief Perform a block-wide reduction
       @param[in] value_ thread-local value to be reduced
       @param[in] async Whether this reduction will be performed
       asynchronously with respect to the calling threads
       @param[in] batch The batch index of the reduction
       @param[in] all Whether we want all threads to have visibility
       to the result (all = true) or just the first thread in the
       block (all = false)
       @param[in] r The reduction operation we want to apply
       @return The block-wide reduced value
     */
    template <typename T, typename reducer_t, typename param_t, typename ...O>
    inline T operator()(const T &value_, bool async, int batch, bool all,
			const reducer_t &r, const param_t &, O *...ops)
    {
      constexpr auto max_items = device::max_block_size() / device::warp_size();
      const auto thread_idx = target::thread_idx_linear<param_t::block_dim>();
      const auto block_size = target::block_size<param_t::block_dim>();
      const auto warp_idx = thread_idx / device::warp_size();
      const auto warp_items = (block_size + device::warp_size() - 1) / device::warp_size();

      // first do warp reduce
      T value = warp_reduce<true>()(value_, false, r, warp_reduce_param<device::warp_size()>());

      if (!all && warp_items == 1) return value; // short circuit for single warp CTA

      // now do reduction between warps
      if (!async) __syncthreads(); // only synchronize if we are not pipelining

      //__shared__ T storage[max_items];
      T *storage = nullptr;
      //if constexpr (std::is_same_v<std::tuple_element_t<0, std::tuple<BR...,void>>,void>) {
      if constexpr (sizeof...(O) == 0 || std::is_same_v<O...,void>) {
	static_assert(sizeof(T[max_items])<=device::shared_memory_size(), "Block reduce shared mem size too large");
	auto mem = sycl::ext::oneapi::group_local_memory_for_overwrite<T[max_items]>(getGroup());
	storage = *mem.get();
      } else {
	//storage = getSpecialOp<only_SharedMemory<T>>((ops,...)).getSharedMemPtr();
	//storage = getSpecialOp<only_BlockReduce<T>>((ops,...)).getSharedMemPtr();
	auto brops = getDependentOps<only_BlockReduce<T>>((ops,...));
	storage = getSharedMemPtr(getSpecialOp<only_SharedMemory<T>>(brops));  // FIXME: should be custom size: /warpsize
      }

      // if first thread in warp, write result to shared memory
      if (thread_idx % device::warp_size() == 0) storage[batch * warp_items + warp_idx] = value;
      __syncthreads();

      // whether to use the first warp or first thread for the final reduction
      constexpr bool final_warp_reduction = true;

      if constexpr (final_warp_reduction) { // first warp completes the reduction (requires first warp is full)
        if (warp_idx == 0) {
          if constexpr (max_items > device::warp_size()) { // never true for max block size 1024, warp = 32
            value = r.init();
            for (auto i = thread_idx; i < warp_items; i += device::warp_size())
              value = r(storage[batch * warp_items + i], value);
          } else { // optimized path where we know the final reduction will fit in a warp
            value = thread_idx < warp_items ? storage[batch * warp_items + thread_idx] : r.init();
          }
          value = warp_reduce<true>()(value, false, r, warp_reduce_param<device::warp_size()>());
        }
      } else { // first thread completes the reduction
        if (thread_idx == 0) {
          for (unsigned int i = 1; i < warp_items; i++) value = r(storage[batch * warp_items + i], value);
        }
      }

      if (all) {
        if (thread_idx == 0) storage[batch * warp_items + 0] = value;
        __syncthreads();
        value = storage[batch * warp_items + 0];
      }

      return value;
    }
  };

  //template <> struct block_reduce<true> : block_reduceG {};
  template <> struct block_reduce<true> : block_reduceW {};

} // namespace quda

#include "../generic/block_reduce_helper.h"
