#include <gauge_field.h>
#include <color_spinor_field.h>
#include <clover_field.h>
#include <dslash.h>
#include <worker.h>

#include <dslash_policy.cuh>
#include <kernels/dslash_ndeg_twisted_clover.cuh>

/**
   This is the gauged non-degenerate twisted-clover operator acting on a 
   quark doublet.
*/

namespace quda
{

  template <typename Arg> class NdegTwistedClover : public Dslash<nDegTwistedClover, Arg>
    {
      using Dslash = Dslash<nDegTwistedClover, Arg>;
      using Dslash::arg;
      using Dslash::in;
      
    public:
    NdegTwistedClover(Arg &arg, const ColorSpinorField &out, const ColorSpinorField &in) : Dslash(arg, out, in)
        {
          TunableVectorYZ::resizeVector(2, arg.nParity);
        }
      
      void apply(const qudaStream_t &stream)
      {
        TuneParam tp = tuneLaunch(*this, getTuning(), getVerbosity());
        Dslash::setParam(tp);
        if (arg.xpay)
          Dslash::template instantiate<packShmem, true>(tp, stream);
        else
          errorQuda("Non-degenerate twisted-clover operator only defined for xpay=true");
      }
      
      long long flops() const
      {
        int clover_flops = 504;
        long long flops = Dslash::flops();
        switch (arg.kernel_type) {
        case INTERIOR_KERNEL:
        case KERNEL_POLICY:
          // b and c multiply (= 2 * 48 * in.Volume())
          flops += 2 * in.Ncolor() * 4 * 4 * in.Volume(); // complex * Nc * Ns * fma * vol
          flops += clover_flops * in.Volume();
          break;
        default: break; // twisted-mass flops are in the interior kernel
        }
        return flops;
      }
      long long bytes() const
      {
        int clover_bytes = 72 * in.Precision() + (isFixed<typename Arg::Float>::value ? 2 * sizeof(float) : 0);
        
        long long bytes = Dslash::bytes();
        switch (arg.kernel_type) {
        case INTERIOR_KERNEL:
        case KERNEL_POLICY: bytes += clover_bytes * in.Volume(); break;
        default: break;
        }
        
        return bytes;
      }
    };
  
  template <typename Float, int nColor, QudaReconstructType recon> struct NdegTwistedCloverApply {
    
    inline NdegTwistedCloverApply(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U,
                                  const CloverField &C, double a,
                                  double b, double c, const ColorSpinorField &x, int parity, bool dagger,
                                  const int *comm_override, TimeProfile &profile)
    {
      constexpr int nDim = 4;
      NdegTwistedCloverArg<Float, nColor, nDim, recon> arg(out, in, U, C, a, b, c, x, parity, dagger, comm_override);
      NdegTwistedClover<decltype(arg)> twisted(arg, out, in);
      // why not in.VolumeCB() and in. GhostFaceCB() ??
      dslash::DslashPolicyTune<decltype(twisted)> policy(
        twisted, const_cast<cudaColorSpinorField *>(static_cast<const cudaColorSpinorField *>(&in)),
        in.getDslashConstant().volume_4d_cb, in.getDslashConstant().ghostFaceCB, profile);
      policy.apply(0);
    }
  };
  
  void ApplyNdegTwistedClover(ColorSpinorField &out, const ColorSpinorField &in, const GaugeField &U, const CloverField &C,
                              double a, double b,
                              double c, const ColorSpinorField &x, int parity, bool dagger, const int *comm_override,
                              TimeProfile &profile)
  {
#ifdef GPU_NDEG_TWISTED_CLOVER_DIRAC
    instantiate<NdegTwistedCloverApply>(out, in, U, C, a, b, c, x, parity, dagger, comm_override, profile);
#else
    errorQuda("Non-degenerate twisted-clover dslash has not been built");
#endif // GPU_NDEG_TWISTED_CLOVER_DIRAC
  }
  
} // namespace quda
