#include <color_spinor_field.h>
#include <color_spinor_field_order.h>
#include <tune_quda.h>
#include <cub/cub.cuh>
#include <typeinfo>
#include <multigrid_helper.cuh>

namespace quda {

#ifdef GPU_MULTIGRID

  using namespace quda::colorspinor;

  /** 
      Kernel argument struct
  */
  template <typename Out, typename In, typename Rotator, int fineSpin, int coarseSpin>
  struct RestrictArg {
    Out out;
    const In in;
    const Rotator V;
    const int *fine_to_coarse;
    const int *coarse_to_fine;
    const spin_mapper<fineSpin,coarseSpin> spin_map;

    RestrictArg(Out &out, const In &in, const Rotator &V,
		const int *fine_to_coarse, const int *coarse_to_fine) : 
      out(out), in(in), V(V), fine_to_coarse(fine_to_coarse), coarse_to_fine(coarse_to_fine), spin_map()
    { }

    RestrictArg(const RestrictArg<Out,In,Rotator,fineSpin,coarseSpin> &arg) :
      out(arg.out), in(arg.in), V(arg.V), 
      fine_to_coarse(arg.fine_to_coarse), coarse_to_fine(arg.coarse_to_fine), spin_map()
    { }
  };


  /**
     Rotates from the fine-color basis into the coarse-color basis.
     A.S.: also works for staggered (fineSpin = 1)
  */
  template <typename Float, int fineSpin, int fineColor, int coarseColor, class FineColor, class Rotator>
  __device__ __host__ inline void rotateCoarseColor(complex<Float> out[fineSpin*coarseColor],
						    const FineColor &in, const Rotator &V, int parity, int x_cb) {
    for (int s=0; s<fineSpin; s++)
      for (int i=0; i<coarseColor; i++) out[s*coarseColor+i] = 0.0;

    for (int i=0; i<coarseColor; i++) { // coarse color
      for (int s=0; s<fineSpin; s++) {
	for (int j=0; j<fineColor; j++) {
	  out[s*coarseColor + i] += conj(V(parity, x_cb, s, j, i)) * in(parity, x_cb, s, j);
	}
      }
    }
  }

  template <typename Float, int fineSpin, int fineColor, int coarseSpin, int coarseColor, typename Arg>
  void Restrict(Arg arg) {
    for (int parity_coarse=0; parity_coarse<2; parity_coarse++) 
      for (int x_coarse_cb=0; x_coarse_cb<arg.out.VolumeCB(); x_coarse_cb++)
	for (int s=0; s<coarseSpin; s++) 
	  for (int c=0; c<coarseColor; c++)
	    arg.out(parity_coarse, x_coarse_cb, s, c) = 0.0;

    // loop over fine degrees of freedom
    for (int parity=0; parity<2; parity++) {
      for (int x_cb=0; x_cb<arg.in.VolumeCB(); x_cb++) {
	complex<Float> tmp[fineSpin*coarseColor];
	rotateCoarseColor<Float,fineSpin,fineColor,coarseColor>(tmp, arg.in, arg.V, parity, x_cb);

	int x = parity*arg.in.VolumeCB() + x_cb;
	int x_coarse = arg.fine_to_coarse[x];
	int parity_coarse = (x_coarse >= arg.out.VolumeCB()) ? 1 : 0;
	int x_coarse_cb = x_coarse - parity_coarse*arg.out.VolumeCB();
	
        if(fineSpin == 1)
        {
           int staggered_coarse_spin = parity; //0 if fine parity even, 1 otherwise
           for (int c=0; c<coarseColor; c++)
	      arg.out(parity_coarse,x_coarse_cb,staggered_coarse_spin,c) += tmp[c];
        }
        else
        {
	  for (int s=0; s<fineSpin; s++) 
	    for (int c=0; c<coarseColor; c++)
	      arg.out(parity_coarse,x_coarse_cb,arg.spin_map(s),c) += tmp[s*coarseColor+c];
        }
      }
    }

    return;
  }

  /**
     Here, we ensure that each thread block maps exactly to a
     geometric block.  Each thread block corresponds to one geometric
     block, with number of threads equal to the number of fine grid
     points per aggregate, so each thread represents a fine-grid
     point.  The look up table coarse_to_fine is the mapping to the
     each fine grid point.
  */
  template <typename Float, int fineSpin, int fineColor, int coarseSpin, int coarseColor, typename Arg, int block_size>
  __global__ void RestrictKernel(Arg arg) {
    int x_coarse = blockIdx.x;
    int parity_coarse = x_coarse >= arg.out.VolumeCB() ? 1 : 0;
    int x_coarse_cb = x_coarse - parity_coarse*arg.out.VolumeCB();

    // obtain fine index from this look up table
    // since both parities map to the same block, each thread block must do both parities

    // threadIdx.x - fine checkboard offset
    // threadIdx.y - fine parity offset
    // blockIdx.x  - which coarse block are we working on
    // assume that coarse_to_fine look up map is ordered as (coarse-block-id + fine-point-id)
    // and that fine-point-id is parity ordered
    int x_fine = arg.coarse_to_fine[ (blockIdx.x*blockDim.y + threadIdx.y) * blockDim.x + threadIdx.x];
    int parity = threadIdx.y;
    int x_fine_cb = x_fine - parity*arg.in.VolumeCB();

    complex<Float> tmp[fineSpin*coarseColor];
    rotateCoarseColor<Float,fineSpin,fineColor,coarseColor>(tmp, arg.in, arg.V, parity, x_fine_cb);

    complex<Float> reduced[coarseSpin * coarseColor];
    for (int i=0; i<coarseSpin*coarseColor; i++) reduced[i] = 0.0;//Why the class constructor does not initialize it to zero?

    if(fineSpin != 1)
    {
      // first lets coarsen spin locally
      for (int s=0; s<fineSpin; s++) {
        for (int v=0; v<coarseColor; v++) {
	  reduced[arg.spin_map(s)*coarseColor+v] += tmp[s*coarseColor+v];
        }
      }

      // now lets coarse geometry across threads
      typedef cub::BlockReduce<complex<Float>, block_size, cub::BLOCK_REDUCE_WARP_REDUCTIONS, 2> BlockReduce;
      __shared__ typename BlockReduce::TempStorage temp_storage;
      for (int s=0; s<coarseSpin; s++) {
        for (int v=0; v<coarseColor; v++) {
	  reduced[s*coarseColor+v] = BlockReduce(temp_storage).Sum( reduced[s*coarseColor+v] );
	  __syncthreads();
        }
      }
    }
    else//staggered block (temporary hack)
    {
      
      for (int s=0; s<coarseSpin; s++) {
        for (int v=0; v<coarseColor; v++) {
	  reduced[s*coarseColor+v] += (s == parity) ? tmp[v] : 0.0;
        }
      }

      // now lets coarse geometry across threads
      typedef cub::BlockReduce<complex<Float>, block_size, cub::BLOCK_REDUCE_WARP_REDUCTIONS, 2> BlockReduce;
      __shared__ typename BlockReduce::TempStorage temp_storage;
      for (int s=0; s<coarseSpin; s++) {
        for (int v=0; v<coarseColor; v++) {
	  reduced[s*coarseColor+v] = BlockReduce(temp_storage).Sum( reduced[s*coarseColor+v] );
	  __syncthreads();
        }
      }
    }

    if (threadIdx.x==0 && threadIdx.y == 0) {
      for (int s=0; s<coarseSpin; s++) { // hard code coarse spin to 2 for now
	for (int v=0; v<coarseColor; v++) {
	  arg.out(parity_coarse, x_coarse_cb, s, v) = reduced[s*coarseColor+v];
	}
      }

    }

    return;

  }

  template <typename Float, typename Arg, int fineSpin, int fineColor, int coarseSpin, int coarseColor>
  class RestrictLaunch : public Tunable {

  protected:
    Arg &arg;
    QudaFieldLocation location;
    const int block_size;
    char vol[TuneKey::volume_n];

    long long flops() const { return 0; }
    unsigned int sharedBytesPerThread() const { return 0; }
    unsigned int sharedBytesPerBlock(const TuneParam &param) const { return 0; }
    bool tuneGridDim() const { return false; } // Don't tune the grid dimensions.
    unsigned int minThreads() const { return arg.in.VolumeCB(); } // fine parity is the block y dimension

  public:
    RestrictLaunch(Arg &arg, const ColorSpinorField &coarse, const ColorSpinorField &fine, 
		   const QudaFieldLocation location) : arg(arg), location(location), 
	block_size((arg.in.VolumeCB())/arg.out.Volume()) {
      strcpy(vol, coarse.VolString());
      strcat(vol, ",");
      strcat(vol, fine.VolString());

      strcpy(aux, coarse.AuxString());
      strcat(aux, ",");
      strcat(aux, fine.AuxString());
    } // block size is checkerboard fine length / full coarse length
    virtual ~RestrictLaunch() { }

    void apply(const cudaStream_t &stream) {
      if (location == QUDA_CPU_FIELD_LOCATION) {
	Restrict<Float,fineSpin,fineColor,coarseSpin,coarseColor>(arg);
      } else {
	TuneParam tp = tuneLaunch(*this, getTuning(), getVerbosity());
	tp.block.y = 2; // need factor of two for fine parity with in the block

	if (block_size == 8) {
	  RestrictKernel<Float,fineSpin,fineColor,coarseSpin,coarseColor,Arg,8>
	    <<<tp.grid, tp.block, tp.shared_bytes, stream>>>(arg);
	} else if (block_size == 16) {
	  RestrictKernel<Float,fineSpin,fineColor,coarseSpin,coarseColor,Arg,16>
	    <<<tp.grid, tp.block, tp.shared_bytes, stream>>>(arg);
	} else if (block_size == 128) {
	  RestrictKernel<Float,fineSpin,fineColor,coarseSpin,coarseColor,Arg,128>
	  <<<tp.grid, tp.block, tp.shared_bytes, stream>>>(arg);
	} else {
	  errorQuda("Block size %d not instantiated", block_size);
	}
      }
    }

    // only tune shared memory per thread since grid and block sizes are fixed
    bool advanceTuneParam(TuneParam &param) const { return advanceSharedBytes(param); }

    TuneKey tuneKey() const {
      return TuneKey(vol, typeid(*this).name(), aux);
    }

    void initTuneParam(TuneParam &param) const { defaultTuneParam(param); }

    /** sets default values for when tuning is disabled */
    void defaultTuneParam(TuneParam &param) const {
      param.block = dim3(block_size, 1, 1);
      param.grid = dim3( (minThreads()+param.block.x-1) / param.block.x, 1, 1);
      param.shared_bytes = 0;
    }

    long long bytes() const {
      return arg.in.Bytes() + arg.out.Bytes() + arg.V.Bytes() + arg.in.Volume()*sizeof(int);
    }

  };

  template <typename Float, int fineSpin, int fineColor, int coarseSpin, int coarseColor, QudaFieldOrder order>
  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		const int *fine_to_coarse, const int *coarse_to_fine) {

    typedef FieldOrderCB<Float,fineSpin,fineColor,1,order> fineSpinor;
    typedef FieldOrderCB<Float,coarseSpin,coarseColor,1,order> coarseSpinor;
    typedef FieldOrderCB<Float,fineSpin,fineColor,coarseColor,order> packedSpinor;
    typedef RestrictArg<coarseSpinor,fineSpinor,packedSpinor,fineSpin,coarseSpin> Arg;

    coarseSpinor Out(const_cast<ColorSpinorField&>(out));
    fineSpinor   In(const_cast<ColorSpinorField&>(in));
    packedSpinor V(const_cast<ColorSpinorField&>(v));

    Arg arg(Out, In, V, fine_to_coarse,coarse_to_fine);
    RestrictLaunch<Float, Arg, fineSpin, fineColor, coarseSpin, coarseColor> restrictor(arg, out, in, Location(out, in, v));
    restrictor.apply(0);

    if (Location(out, in, v) == QUDA_CUDA_FIELD_LOCATION) checkCudaError();
  }

  template <typename Float, int fineSpin, int fineColor, int coarseSpin, QudaFieldOrder order>
  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		int nVec, const int *fine_to_coarse, const int *coarse_to_fine, const int *spin_map) {

    // first check that the spin_map matches the spin_mapper
    if(spin_map != NULL) //spin_map is undefined for the top level staggered fermions.
    {
      spin_mapper<fineSpin,coarseSpin> mapper;
      for (int s=0; s<fineSpin; s++) 
        if (mapper(s) != spin_map[s]) errorQuda("Spin map does not match spin_mapper");
    }
 
    if (nVec == 2) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,2,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 4) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,4,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 8) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,8,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 12) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,12,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 16) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,16,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 20) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,20,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 24) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,24,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 48) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,48,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else if (nVec == 96) {
      Restrict<Float,fineSpin,fineColor,coarseSpin,96,order>(out, in, v, fine_to_coarse, coarse_to_fine);
    } else {
      errorQuda("Unsupported nVec %d", nVec);
    }
  }

  template <typename Float, int fineSpin, QudaFieldOrder order>
  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		int Nvec, const int *fine_to_coarse, const int *coarse_to_fine, const int *spin_map) {
    if (out.Nspin() != 2) errorQuda("coarseSpin is not supported");

    if (out.Nspin() != 2) errorQuda("Unsupported nSpin %d", out.Nspin());

    if (in.Ncolor() == 3) {
      Restrict<Float,fineSpin,3, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Ncolor() == 2) {
      Restrict<Float,fineSpin,2, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Ncolor() == 8) {
      Restrict<Float,fineSpin,8, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Ncolor() == 16) {
      Restrict<Float,fineSpin,16, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Ncolor() == 24) {
      Restrict<Float,fineSpin,24, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Ncolor() == 48) {
      Restrict<Float,fineSpin,48, 2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else {
      errorQuda("Unsupported nColor %d", in.Ncolor());
    }
  }

  template <typename Float, QudaFieldOrder order>
  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		int Nvec, const int *fine_to_coarse, const int *coarse_to_fine, const int *spin_map) {

    if (in.Nspin() == 4) {
      Restrict<Float,4,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (in.Nspin() == 2) {
      Restrict<Float,2,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
#if GPU_STAGGERED_DIRAC
    } else if (in.Nspin() == 1) {
      Restrict<Float,1,order>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
#endif
    } else {
      errorQuda("Unsupported nSpin %d", in.Nspin());
    }
  }


  template <typename Float>
  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		int Nvec, const int *fine_to_coarse, const int *coarse_to_fine, const int *spin_map) {

    if (out.FieldOrder() != in.FieldOrder() ||	out.FieldOrder() != v.FieldOrder())
      errorQuda("Field orders do not match (out=%d, in=%d, v=%d)", 
		out.FieldOrder(), in.FieldOrder(), v.FieldOrder());

    if (out.FieldOrder() == QUDA_FLOAT2_FIELD_ORDER) {
      Restrict<Float,QUDA_FLOAT2_FIELD_ORDER>
	(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (out.FieldOrder() == QUDA_SPACE_SPIN_COLOR_FIELD_ORDER) {
      Restrict<Float,QUDA_SPACE_SPIN_COLOR_FIELD_ORDER>
	(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else {
      errorQuda("Unsupported field type %d", out.FieldOrder());
    }
  }

#endif // GPU_MULTIGRID

  void Restrict(ColorSpinorField &out, const ColorSpinorField &in, const ColorSpinorField &v,
		int Nvec, const int *fine_to_coarse, const int *coarse_to_fine, const int *spin_map) {

#ifdef GPU_MULTIGRID
    if (out.Precision() != in.Precision() || v.Precision() != in.Precision())
      errorQuda("Precision mismatch out=%d in=%d v=%d", out.Precision(), in.Precision(), v.Precision());

    if (out.Precision() == QUDA_DOUBLE_PRECISION) {
      Restrict<double>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else if (out.Precision() == QUDA_SINGLE_PRECISION) {
      Restrict<float>(out, in, v, Nvec, fine_to_coarse, coarse_to_fine, spin_map);
    } else {
      errorQuda("Unsupported precision %d", out.Precision());
    }
#else
    errorQuda("Multigrid has not been built");
#endif
  }

} // namespace quda
