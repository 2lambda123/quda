#pragma once
#include <tunable_kernel.h>
#include <reduce_helper.h>
#include <timer.h>
#include <quda_sycl_api.h>

//#define HIGH_LEVEL_REDUCTIONS

namespace quda {

#ifndef HIGH_LEVEL_REDUCTIONS
  template <template <typename> class Functor, typename Arg, bool grid_stride = true>
  void Reduction2DImpl(const Arg &arg, const sycl::nd_item<3> &ndi, char *smem)
  {
    Functor<Arg> t(arg);
    reduceSpecialOps<typename Functor<Arg>::reduce_t> rso;
    rso.setNdItem(ndi);
    rso.setSharedMem(smem);
    auto idx = globalIdX;
    auto j = localIdY;
    auto value = t.init();
    while (idx < arg.threads.x) {
      value = t(value, idx, j);
      if (grid_stride) idx += globalRangeX; else break;
    }
    // perform final inter-block reduction and write out result
    reduce(arg, t, value, 0, rso);
  }
  template <template <typename> class Functor, typename Arg, bool grid_stride = false>
  struct Reduction2DS {
    using SpecialOpsT = reduceSpecialOps<typename Functor<Arg>::reduce_t>;
    template <typename... T>
    Reduction2DS(const Arg &arg, const sycl::nd_item<3> &ndi, T ...smem)
    {
#ifdef QUDA_THREADS_BLOCKED
      Reduction2DImpl<Functor,Arg,grid_stride>(arg, ndi);
#else
      Reduction2DImpl<Functor,Arg,grid_stride>(arg, ndi, smem...);
#endif
    }
  };
#else
  template <template <typename> class Functor, bool grid_stride,
	    typename Arg, typename R>
  void Reduction2DImplN(const Arg &arg, const sycl::nd_item<3> &ndi, R &reducer)
  {
    Functor<Arg> t(const_cast<Arg&>(arg));
    auto idx = globalIdX;
    auto j = localIdY;
    auto value = t.init();
    while (idx < arg.threads.x) {
      value = t(value, idx, j);
      if (grid_stride) idx += globalRangeX; else break;
    }
    reducer.combine(value);
  }
  template <template <typename> class Functor, bool grid_stride = false>
  struct Reduction2DS {
    template <typename Arg, typename R>
    static void apply(const Arg &arg, const sycl::nd_item<3> &ndi, R &reducer)
    {
#ifdef QUDA_THREADS_BLOCKED
      Reduction2DImplN<Functor,grid_stride>(arg, ndi, reducer);
#else
      Reduction2DImplN<Functor,grid_stride>(arg, ndi, reducer);
#endif
    }
  };
#endif
  template <template <typename> class Functor, typename Arg, bool grid_stride = true>
  qudaError_t Reduction2D(const TuneParam &tp,
			  const qudaStream_t &stream, Arg &arg)
  {
    static_assert(!hasSpecialOps<Functor<Arg>>);
    auto err = QUDA_SUCCESS;
    auto globalSize = globalRange(tp);
    auto localSize = localRange(tp);
    if (localSize[RANGE_X] % device::warp_size() != 0) {
      return QUDA_ERROR;
    }
#if 0
    if (localSize[RANGE_X] > arg.threads.x) {
      localSize[RANGE_X] = arg.threads.x;
      globalSize[RANGE_X] = arg.threads.x;
    } else if (grid_stride) {
      if (globalSize[RANGE_X] > arg.threads.x) {
	globalSize[RANGE_X] = ((arg.threads.x+localSize[RANGE_X]-1)/localSize[RANGE_X])*localSize[RANGE_X];
      }
    } else {
      if (globalSize[RANGE_X] != arg.threads.x) {
	globalSize[RANGE_X] = ((arg.threads.x+localSize[RANGE_X]-1)/localSize[RANGE_X])*localSize[RANGE_X];
      }
    }
#endif
    host_timer_t timer;
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      printfQuda("Reduction2D grid_stride: %s  sizeof(arg): %lu\n",
		 grid_stride?"true":"false", sizeof(arg));
      printfQuda("  global: %s  local: %s  threads: %s\n", str(globalSize).c_str(),
		 str(localSize).c_str(), str(arg.threads).c_str());
      printfQuda("  Arg: %s\n", typeid(Arg).name());
      printfQuda("  SLM size: %lu\n",
                 localSize.size()*sizeof(typename Functor<Arg>::reduce_t)/
		 device::warp_size());
      printfQuda("  SpecialOps: %s\n", typeid(getSpecialOps<Functor<Arg>>).name());
      printfQuda("  needsFullBlock: %i  needsSharedMem: %i\n", needsFullBlock<Functor<Arg>>, needsSharedMem<Functor<Arg>>);
      printfQuda("  shared_bytes: %i\n", tp.shared_bytes);
      timer.start();
    }
    //if (arg.threads.x%localSize[RANGE_X] != 0) {
      //warningQuda("arg.threads.x (%i) %% localSize X (%lu) != 0", arg.threads.x, localSize[RANGE_X]);
    //  return QUDA_ERROR;
    //}
    //if (globalSize[RANGE_Y] != arg.threads.y) { // shouldn't happen here
      //warningQuda("globalSize Y (%lu) != arg.threads.y (%i)", globalSize[RANGE_Y], arg.threads.y);
    //  return QUDA_ERROR;
    //}
    sycl::nd_range<3> ndRange{globalSize, localSize};
#ifndef HIGH_LEVEL_REDUCTIONS
    err = launch<Reduction2DS<Functor, Arg, grid_stride>>(stream, ndRange, arg);
#else
    err = launchR<Functor, Reduction2DS<Functor, grid_stride>>(stream, ndRange, arg);
#endif
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      timer.stop();
      //printfQuda("  launch time: %g\n", timer.last());
      //auto q = device::get_target_stream(stream);
      //using reduce_t = typename Functor<Arg>::reduce_t;
      //if (commAsyncReduction()) {
      //q.memcpy(result_h, result_d, sizeof(reduce_t));
      //}
      //q.wait_and_throw();
      //printfQuda("end Reduction2D result_h: %g\n", *(double *)result_h);
      printfQuda("end Reduction2D launch time: %g\n", timer.last());
    }
    return err;
  }

  // MultiReduction

  template <template <typename> class Functor, typename Arg, bool grid_stride = true, typename S>
  void MultiReductionImpl(const Arg &arg, const sycl::nd_item<3> &ndi, S smem)
  {
    using reduce_t = typename Functor<Arg>::reduce_t;
    Functor<Arg> t(arg);
    reduceSpecialOps<typename Functor<Arg>::reduce_t> rso;
    rso.setNdItem(ndi);
    rso.setSharedMem(smem);

    auto idx = globalIdX;
    auto k = localIdY;
    auto j = globalIdZ;

    //if (j >= arg.threads.z) return;

    reduce_t value = t.init();

    if (j < arg.threads.z) {
      while (idx < arg.threads.x) {
	value = t(value, idx, k, j);
	if (grid_stride) idx += globalRangeX; else break;
      }
    }

    // perform final inter-block reduction and write out result
    reduce(arg, t, value, j, rso);
  }
  template <template <typename> class Functor, typename Arg, bool grid_stride>
  struct MultiReductionS {
    using SpecialOpsT = reduceSpecialOps<typename Functor<Arg>::reduce_t>;
    template <typename... T>
    MultiReductionS(const Arg &arg, const sycl::nd_item<3> &ndi, T... smem)
    {
#ifdef QUDA_THREADS_BLOCKED
      MultiReductionImpl<Functor,Arg,grid_stride>(arg, ndi);
#else
      MultiReductionImpl<Functor,Arg,grid_stride>(arg, ndi, smem...);
#endif
    }
  };

  template <template <typename> class Functor, typename Arg, bool grid_stride = true>
  qudaError_t
  MultiReduction(const TuneParam &tp, const qudaStream_t &stream, Arg &arg)
  {
    static_assert(!hasSpecialOps<Functor<Arg>>);
    auto err = QUDA_SUCCESS;
    auto globalSize = globalRange(tp);
    auto localSize = localRange(tp);
    if (localSize[RANGE_X] % device::warp_size() != 0) {
      return QUDA_ERROR;
    }
#if 0
    if (localSize[RANGE_X] > arg.threads.x) {
      localSize[RANGE_X] = arg.threads.x;
      globalSize[RANGE_X] = arg.threads.x;
    } else if (grid_stride) {
      if (globalSize[RANGE_X] > arg.threads.x) {
	globalSize[RANGE_X] = ((arg.threads.x+localSize[RANGE_X]-1)/localSize[RANGE_X])*localSize[RANGE_X];
      }
    } else {
      if (globalSize[RANGE_X] != arg.threads.x) {
	globalSize[RANGE_X] = ((arg.threads.x+localSize[RANGE_X]-1)/localSize[RANGE_X])*localSize[RANGE_X];
      }
    }
#endif
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      using reduce_t = typename Functor<Arg>::reduce_t;
      printfQuda("MultiReduction grid_stride: %s  sizeof(arg): %lu\n",
		 grid_stride?"true":"false", sizeof(arg));
      printfQuda("  global: %s  local: %s  threads: %s\n", str(globalSize).c_str(),
		 str(localSize).c_str(), str(arg.threads).c_str());
      printfQuda("  Arg: %s\n", typeid(Arg).name());
      printfQuda("  reduce_t: %s\n", typeid(reduce_t).name());
      printfQuda("  Arg::max_n_batch_block: %d\n", Arg::max_n_batch_block);
      printfQuda("  Functor::reduce_block_dim: %d\n", Functor<Arg>::reduce_block_dim);
      printfQuda("  max_block_z: %d\n",
		 device::max_block_size()/ (tp.block.x * tp.block.y));
      printfQuda("  SLM size: %lu\n",
                 localSize.size()*sizeof(typename Functor<Arg>::reduce_t)/
		 device::warp_size());
      printfQuda("  SpecialOps: %s\n", typeid(getSpecialOps<Functor<Arg>>).name());
      printfQuda("  needsFullBlock: %i  needsSharedMem: %i\n", needsFullBlock<Functor<Arg>>, needsSharedMem<Functor<Arg>>);
      printfQuda("  shared_bytes: %i\n", tp.shared_bytes);
    }
    //if (arg.threads.x%localSize[RANGE_X] != 0) {
      //warningQuda("arg.threads.x (%i) %% localSize X (%lu) != 0", arg.threads.x, localSize[RANGE_X]);
    //  return QUDA_ERROR;
    //}
    //if (globalSize[RANGE_Y] != arg.threads.y) { // shouldn't happen here
      //warningQuda("globalSize Y (%lu) != arg.threads.y (%i)", globalSize[RANGE_Y], arg.threads.y);
    //  return QUDA_ERROR;
    //}
    //if (globalSize[RANGE_Z] != arg.threads.z) {
      //warningQuda("globalSize Z (%lu) != arg.threads.z (%i)", globalSize[RANGE_Z], arg.threads.z);
    //  return QUDA_ERROR;
    //}
    sycl::nd_range<3> ndRange{globalSize, localSize};
    err = launch<MultiReductionS<Functor, Arg, grid_stride>>(stream, ndRange, arg);
    //err = launchX<MultiReductionS<Functor, Arg, grid_stride>>(stream, ndRange, arg);
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      printfQuda("end MultiReduction\n");
    }
    return err;
  }

}