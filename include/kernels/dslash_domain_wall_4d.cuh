#pragma once

#include <kernels/dslash_wilson.cuh>

#include <mma_tensor_op/gemm.cuh>

#include <cuda/barrier>
#include <cooperative_groups.h>

namespace quda
{

  template <typename Float, int nColor, int nDim, QudaReconstructType reconstruct_>
  struct DomainWall4DArg : WilsonArg<Float, nColor, nDim, reconstruct_> {
    typedef typename mapper<Float>::type real;
    int Ls;                             /** fifth dimension length */
    complex<real> a_5[QUDA_MAX_DWF_LS]; /** xpay scale factor for each 4-d subvolume */

    DomainWall4DArg(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, double a, double m_5,
                    const Complex *b_5, const Complex *c_5, bool xpay, const ColorSpinorField &x, int parity,
                    bool dagger, const int *comm_override) :
      WilsonArg<Float, nColor, nDim, reconstruct_>(out, in, U, xpay ? a : 0.0, x, parity, dagger, comm_override),
      Ls(in.X(4))
    {
      if (b_5 == nullptr || c_5 == nullptr)
        for (int s = 0; s < Ls; s++) a_5[s] = a; // 4-d Shamir
      else
        for (int s = 0; s < Ls; s++) a_5[s] = 0.5 * a / (b_5[s] * (m_5 + 4.0) + 1.0); // 4-d Mobius
    }
  };

  __device__ float truncated_multiply(float a, float b) {
    constexpr unsigned int mask = 0xFFFFE000; // 1111 1111 1111 1110 0000 0000 0000
    float a_t = __uint_as_float(__float_as_uint(a) & mask);
    float b_t = __uint_as_float(__float_as_uint(b) & mask);
    return a_t * b_t;
  }

  /**
     @brief Compute the checkerboard 1-d index for the nearest
     neighbor
     @param[in] lattice coordinates
     @param[in] mu dimension in which to add 1
     @param[in] dir direction (+1 or -1)
     @param[in] arg parameter struct
     @return 1-d checkboard index
   */
  template <typename Coord, typename Arg>
  __device__ __host__ inline int getNeighborIndexCB_dyn(const Coord &x, int mu, int dir, const Arg &arg)
  {
    int rtn = (x[mu] == ((dir + 1) / 2) * (arg.X[mu] - 1) ? x.X - dir * arg.mZ[mu] : x.X + dir * arg.Z[mu]) >> 1;
    return rtn;
  }

  using barrier = cuda::barrier<cuda::thread_scope_block>;

  constexpr int warp_size = 32;

  template <int Ls>
  __device__ int inline s_spin_index(int s, int spin, int comp) {
    return spin * (2 * Ls) + s * 2 + comp;
  }

  template <bool dagger, class S, class link_t>
  __device__ void inline store_link(int buffer_index, int d, S smem_obj, const link_t &U)
  {
#pragma unroll
    for (int ci = 0; ci < 3; ci++) {
#pragma unroll
      for (int cj = 0; cj < 3; cj++) { // ind_n, ind_k
        smem_obj(cj * 2 + 0, ci + (d * 2 + (dagger ? 0 : 0)) * 3 + buffer_index * 3) = dagger ? +U(ci, cj).real() : +U(cj, ci).real();
        smem_obj(cj * 2 + 1, ci + (d * 2 + (dagger ? 0 : 0)) * 3 + buffer_index * 3) = dagger ? -U(ci, cj).imag() : +U(cj, ci).imag();
      }
    }
  }

  template <int Ls, class S, class colorspinor_t>
    __device__ void inline store_colorspinor(int buffer_index, int d, int s, int fwd_bwd, S smem_obj, const colorspinor_t &in_p)
    {
#pragma unroll
      for (int spin = 0; spin < 4; spin++) {
#pragma unroll
        for (int color = 0; color < 3; color++) {
          smem_obj(s_spin_index<Ls>(s, spin, 0), color + (d * 2 + fwd_bwd) * 3 + buffer_index * 3) = in_p(spin, color).real();
          smem_obj(s_spin_index<Ls>(s, spin, 1), color + (d * 2 + fwd_bwd) * 3 + buffer_index * 3) = in_p(spin, color).imag();
        }
      }
    }

  template <int Ls, class S, class colorspinor_t>
    __device__ void inline load_colorspinor(int buffer_index, int s, colorspinor_t &out, const S smem_obj)
  {
#pragma unroll
    for (int spin = 0; spin < 4; spin++) {
#pragma unroll
      for (int color = 0; color < 3; color++) {
        float_t rr = smem_obj(s_spin_index<Ls>(s, spin, 0), color * 2 + 0 + buffer_index * 6);
        float_t ir = smem_obj(s_spin_index<Ls>(s, spin, 1), color * 2 + 0 + buffer_index * 6);
        float_t ri = smem_obj(s_spin_index<Ls>(s, spin, 0), color * 2 + 1 + buffer_index * 6);
        float_t ii = smem_obj(s_spin_index<Ls>(s, spin, 1), color * 2 + 1 + buffer_index * 6);

        out(spin, color).real(rr - ii);
        out(spin, color).imag(ri + ir);
      }
    }
  }

  template <int Ls, int smem_buffer_size, int nParity, bool dagger, bool xpay, KernelType kernel_type, class A, class B, class C, typename Arg>
  __device__ inline void data_movement(A smem_obj_a, B smem_obj_b, C smem_obj_c, Arg &arg)
  {
    static_assert(kernel_type == INTERIOR_KERNEL, "Currently only for interior kernel.");

    int thread_idx = threadIdx.x + blockDim.x * threadIdx.y;
    int s = thread_idx % Ls;
    int y = thread_idx / Ls;
    int x_cb = y + blockIdx.x * blockDim.y;

    constexpr int Ns = Arg::nSpin;
    constexpr int Nc = Arg::nColor;

    // if (blockIdx.x == 0) { printf("trd = %3d, x_cb = %5d, s = %2d\n", thread_idx, x_cb, s); }

    using float_t = typename mapper<typename Arg::Float>::type;
    using fixed_t = typename Arg::Float;
    using colorspinor_t = ColorSpinor<float_t, Nc, Ns>;
    using link_t = Matrix<complex<float_t>, Nc>;

    int parity = nParity == 2 ? blockDim.z * blockIdx.z + threadIdx.z : arg.parity;
    int their_spinor_parity = nParity == 2 ? 1 - parity : 0;

    const int gauge_parity = parity;

    int thread_dim;
    bool active;
    auto coord = getCoords<QUDA_4D_PC, kernel_type>(arg, x_cb, s, parity, thread_dim);

    int buffer_index = y % smem_buffer_size;

#pragma unroll
    for (int d = 0; d < 4; d++) { // loop over dimension - 4 and not nDim since this is used for DWF as well
      {                           // Forward gather - compute fwd offset for vector fetch
        const int fwd_idx = getNeighborIndexCB(coord, d, +1, arg.dc);
        const int gauge_idx = coord.x_cb;
        constexpr int proj_dir = dagger ? +1 : -1;

        const bool ghost
          = (coord[d] + arg.nFace >= arg.dim[d]) && isActive<kernel_type>(active, thread_dim, d, coord, arg);

        if (!ghost) { // TODO: zero the ghost ones.
          // colorspinor_t in = arg.in(fwd_idx * Ls + s, their_spinor_parity);
          colorspinor_t in = arg.in(fwd_idx + coord.s * arg.dc.volume_4d_cb, their_spinor_parity);
          colorspinor_t in_p = in.project(d, proj_dir).reconstruct(d, proj_dir);
          link_t U;
          if (s == 0) {
            U = arg.U(d, gauge_idx, gauge_parity);
          }

          if (d > 0) { __syncthreads(); } // "The buffers are filled."
          // ready[y].arrive_and_wait(); // Wait until buffer for `x_cb` is ready to be filled.

          if (s == 0) {
            store_link<false>(buffer_index, 0, smem_obj_b, U); // dagger = false
          }
          store_colorspinor<Ls>(buffer_index, 0, s, 0, smem_obj_a, in_p);
          // out += (U * in.project(d, proj_dir)).reconstruct(d, proj_dir);
          
          __syncthreads(); // "The buffers are filled."
          // filled[y].arrive(); // Signal that the op_a and op_b for this `x_cb` are now filled
        }
      }

      { // Backward gather - compute back offset for spinor and gauge fetch
        const int back_idx = getNeighborIndexCB(coord, d, -1, arg.dc);
        const int gauge_idx = back_idx;
        constexpr int proj_dir = dagger ? -1 : +1;

        const bool ghost = (coord[d] - arg.nFace < 0) && isActive<kernel_type>(active, thread_dim, d, coord, arg);

        if (!ghost) {
          // colorspinor_t in = arg.in(back_idx * Ls + s, their_spinor_parity);
          colorspinor_t in = arg.in(back_idx + coord.s * arg.dc.volume_4d_cb, their_spinor_parity);
          colorspinor_t in_p = in.project(d, proj_dir).reconstruct(d, proj_dir);
          link_t U;
          if (s == 0) {
            U = arg.U(d, gauge_idx, 1 - gauge_parity);
          }

          __syncthreads(); // "The buffers are ready to be filled."
          // ready[y].arrive_and_wait(); // Wait until buffer for `x_cb` is ready to be filled.

          if (s == 0) {
            store_link<true>(buffer_index, 0, smem_obj_b, U); // dagger = true
          }
          store_colorspinor<Ls>(buffer_index, 0, s, 0, smem_obj_a, in_p);
          // out += (conj(U) * in.project(d, proj_dir)).reconstruct(d, proj_dir);

          __syncthreads(); // "The buffers are filled."
          // filled[y].arrive(); // Signal that the op_a and op_b for this `x_cb` are now filled
        }
      }
    } // nDim

    __syncthreads(); // "Compute is done."
    // computed[y].arrive_and_wait(); // Wait for the results for this `x_cb` have been computed.

    // Write output
    const int my_spinor_parity = nParity == 2 ? parity : 0;
    colorspinor_t out;
    load_colorspinor<Ls>(buffer_index, s, out, smem_obj_c);
    // arg.out(coord.x_cb * Ls + s, my_spinor_parity) = out;
    arg.out(coord.x_cb + s * arg.dc.volume_4d_cb, my_spinor_parity) = out;
  }

  template <int smem_buffer_size, int Ls, int block_y, class A, class B, class C>
  __device__ inline void compute(int thread, A smem_obj_a, B smem_obj_b, C smem_obj_c)
  {
    constexpr int Nc = 3;
    constexpr int Ns = 4;

    constexpr int M = Ls * Ns * 2; // Ls * spin * complex
    constexpr int N = Nc * 2;     // Nc * complex
    constexpr int K = Nc * 8; // Nc * 8-way stencil

    constexpr int bM = M;
    constexpr int bN = N; // 8 -> 6
    constexpr int bK = Nc; // TODO: Change this for MMA.

#pragma unroll
    for (int cycle = 0; cycle < block_y / smem_buffer_size; cycle++) {
#pragma unroll
      for (int b = 0; b < smem_buffer_size; b++) {
        // ready[b + cycle * smem_buffer_size].arrive(); // Note that the buffers are now ready to be filled for these `x_cb`.
      }
      for (int kk = 0; kk < K; kk += bK) {

        __syncthreads(); // "The buffers are ready."

        for (int b = 0; b < smem_buffer_size; b++) {

          // filled[b + cycle * smem_buffer_size].arrive_and_wait(); // wait for the buffers to be filled

          for (int m = thread; m < bM; m += 1 * 32) {
            for (int n = 0; n < bN; n++) {
              // if (blockIdx.x == 0 && n == 0) { printf("trd = %2d, m = %2d, n = %2d, b = %d\n", threadIdx.x + blockDim.x * threadIdx.y - 96, m, n, b); }
              float acc = kk == 0 ? 0 : smem_obj_c(m, n + bN * b);
              for (int k = 0; k < bK; k++) { acc += smem_obj_a(m, k + bK * b) * smem_obj_b(n, k + bK * b); }
              smem_obj_c(m, n + bN * b) = acc;
            }
          }

          if (kk + bK < K) {
            // ready[b + cycle * smem_buffer_size].arrive();
          } else {
            // computed[b + cycle * smem_buffer_size].arrive(); // Signal that this part has been computed 
          }
        }

        __syncthreads(); // "The buffers are ready to be filled." or "Compute is done."
      }
    }
  }

  template <int Ls, int nParity, bool dagger, bool xpay, KernelType kernel_type, typename Arg>
  __global__ void specialized_dslash_kernel(Arg arg)
  {
    constexpr int block_x = 12;         // How many 5th dimension sites are there
    constexpr int block_y = 8;          // How many spatial sites are there in this block
    constexpr int smem_buffer_size = 8; // How many buffers are there
    constexpr int compute_warp = 1;     // How many compute/MMA warps are there

    /**
      block_y == blockDim.y
      block_x * block_y == number of threads for data movement
      (blockDim.x - block_x) * blockDim.y == number of threads for compute
      blockDim.x * blockDim.y == total threads
    */

    constexpr int Nc = 3;
    constexpr int Ns = 4;

    constexpr int M = Ls * Ns * 2;  // Ls * spin * complex
    constexpr int N = Nc * 2;       // Nc * complex
    constexpr int K = Nc * 8;       // Nc * 8-way stencil

    constexpr int bM = M;
    constexpr int bN = 6; // 8 -> 6
    constexpr int bK = Nc; // TODO: Change this for MMA.

    extern __shared__ float smem_ptr[];
    mma::SharedMemoryObject<float_t, bM, bK * smem_buffer_size, 1, (bM + 8)> smem_obj_a(reinterpret_cast<float_t *>(smem_ptr));
    mma::SharedMemoryObject<float_t, bN, bK * smem_buffer_size, 1, bN> smem_obj_b(smem_obj_a.ptr + bK * (bM + 8) * smem_buffer_size);
    mma::SharedMemoryObject<float_t, bM, bN * smem_buffer_size, 1, (bM + 4)> smem_obj_c(smem_obj_b.ptr + bK * bN * smem_buffer_size);

    int thread_idx = threadIdx.x + threadIdx.y * blockDim.x;

    if (thread_idx < block_x * block_y) {
      data_movement<block_x, smem_buffer_size, nParity, dagger, xpay, kernel_type>(smem_obj_a, smem_obj_b, smem_obj_c, arg);
    } else {
      compute<smem_buffer_size, block_x, block_y>(thread_idx - block_y * block_x, smem_obj_a, smem_obj_b, smem_obj_c);
    }
  }

  template <int nParity, bool dagger, bool xpay, KernelType kernel_type, typename Arg> struct domainWall4D;

  template <int nParity, bool dagger, bool xpay, KernelType kernel_type, typename Arg>
  struct domainWall4DImpl: public Dslash<domainWall4D, Arg> {

    using Dslash = Dslash<domainWall4D, Arg>;
    using Dslash::arg;
    using Dslash::in;
    using Dslash::out;

    domainWall4DImpl(const ColorSpinorField &out, const ColorSpinorField &in, Arg &arg): Dslash(arg, out, in) {}

    void apply(const qudaStream_t &stream)
    {
      TuneParam tp = tuneLaunch(*this, getTuning(), getVerbosity());
      tp.set_max_shared_bytes = true;
      qudaLaunchKernel(specialized_dslash_kernel<12, nParity, dagger, xpay, kernel_type, Arg>, tp, stream, arg);
    }

    unsigned int sharedBytesPerThread() const { return 0; }
    unsigned int sharedBytesPerBlock(const TuneParam &param) const { return ((arg.dc.Ls * 8 + 8) * 3 + 6 * 3 + (arg.dc.Ls * 8 + 4) * 6) * sizeof(float) * 8; }

    bool advanceTuneParam(TuneParam &param) const {
      if (param.block.x < blockMax()) {
        param.block.x += blockStep();
        return true;
      } else {
        return false;
      }
    }

    virtual int blockStep() const { return 16; }
    virtual int blockMin() const { return 16; }
    virtual int blockMax() const { return 16; }

    void initTuneParam(TuneParam &param) const
    {
      param.block.x = blockMin();
      param.block.y = 8;
      param.block.z = 1;

      param.grid.x = arg.dc.volume_4d_cb / 8;
      param.grid.y = 1;
      param.grid.z = nParity;

      param.shared_bytes = sharedBytesPerBlock(param);
    }

    void defaultTuneParam(TuneParam &param) const { initTuneParam(param); }

    TuneKey tuneKey() const { return TuneKey(in.VolString(), typeid(*this).name(), ","); }

    void preTune() {}  // FIXME - use write to determine what needs to be saved
    void postTune() {} // FIXME - use write to determine what needs to be saved
 
  };

  template <int nParity, bool dagger, bool xpay, KernelType kernel_type, typename Arg>
  struct domainWall4D : dslash_default {

    Arg &arg;
    constexpr domainWall4D(Arg &arg) : arg(arg) { }
    static constexpr const char *filename() { return KERNEL_FILE; } // this file name - used for run-time compilation

    __device__ __host__ inline void operator()(int idx, int s, int parity)
    {
      typedef typename mapper<typename Arg::Float>::type real;
      typedef ColorSpinor<real, Arg::nColor, 4> Vector;

      bool active
        = kernel_type == EXTERIOR_KERNEL_ALL ? false : true; // is thread active (non-trival for fused kernel only)
      int thread_dim;                                        // which dimension is thread working on (fused kernel only)
      auto coord = getCoords<QUDA_4D_PC, kernel_type>(arg, idx, s, parity, thread_dim);

      const int my_spinor_parity = nParity == 2 ? parity : 0;
      Vector out;
      applyWilson<nParity, dagger, kernel_type>(out, arg, coord, parity, idx, thread_dim, active);

      int xs = coord.x_cb + s * arg.dc.volume_4d_cb;
      if (xpay && kernel_type == INTERIOR_KERNEL) {
        Vector x = arg.x(xs, my_spinor_parity);
        out = x + arg.a_5[s] * out;
      } else if (kernel_type != INTERIOR_KERNEL && active) {
        Vector x = arg.out(xs, my_spinor_parity);
        out = x + (xpay ? arg.a_5[s] * out : out);
      }

      if (kernel_type != EXTERIOR_KERNEL_ALL || active) arg.out(xs, my_spinor_parity) = out;
    }

    static constexpr bool has_specialized_kernel() { return sizeof(typename Arg::Float) == 2 && kernel_type == INTERIOR_KERNEL; }

    static void specialized_launch(const qudaStream_t &stream, const ColorSpinorField &out, const ColorSpinorField &in, Arg &arg) {
      domainWall4DImpl<nParity, dagger, xpay, kernel_type, Arg> impl(out, in, arg);
      impl.apply(stream);
    }
  };

} // namespace quda
