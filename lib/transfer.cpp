#include <transfer.h>
#include <blas_quda.h>

#include <transfer.h>
#include <multigrid.h>
#include <malloc_quda.h>

#include <iostream>
#include <algorithm>
#include <vector>

#include <nvToolsExt.h>

namespace quda {

  /*
  * for the staggered case, there is no spin blocking, 
  * however we do even-odd to preserve chirality (that is straightforward)
  */

  Transfer::Transfer(const std::vector<ColorSpinorField*> &B, int Nvec, int *geo_bs, int spin_bs, bool enable_gpu)
    : B(B), Nvec(Nvec), V_h(0), V_d(0), fine_tmp_h(0), fine_tmp_d(0), coarse_tmp_h(0), coarse_tmp_d(0), geo_bs(0),
      fine_to_coarse_h(0), coarse_to_fine_h(0), 
      fine_to_coarse_d(0), coarse_to_fine_d(0), 
      spin_bs(spin_bs), spin_map(0),
      enable_gpu(enable_gpu), use_gpu(enable_gpu) // by default we apply the transfer operator accorting to enable_gpu flag but can be overridden
  {
    int ndim = B[0]->Ndim();
    this->geo_bs = new int[ndim];
    for (int d = 0; d < ndim; d++)
    {
       this->geo_bs[d] = geo_bs[d];
       if(B[0]->Nspin() == 1 && geo_bs[d] < 4) errorQuda("Too small block size in %d direction for the staggered field (must be >= 4) %d\n", d, geo_bs[d]);
    }

    if (B[0]->X(0) == geo_bs[0]) 
      errorQuda("X-dimension length %d cannot block length %d\n", B[0]->X(0), geo_bs[0]);

    for (int d = 0; d < ndim; d++) {
      if ( (B[0]->X(d)/geo_bs[d]+1)%2 == 0)
	errorQuda("Indexing does not (yet) support odd coarse dimensions: X(%d) = %d\n", d, B[d]->X(d)/geo_bs[d]);

      if ( (B[0]->X(d)/geo_bs[d]) * geo_bs[d] != B[0]->X(d) )
	errorQuda("cannot parition dim[%d]=%d with block length %d\n", d, B[0]->X(d), geo_bs[d]);
    }

    char block_str[128];
    sprintf(block_str, "%d", geo_bs[0]);
    for (int d=1; d<ndim; d++) sprintf(block_str, "%s x %d", block_str, geo_bs[d]);
    printfQuda("Transfer: using block size %s\n", block_str);

    // create the storage for the final block orthogonal elements
    ColorSpinorParam param(*B[0]); // takes the geometry from the null-space vectors

    // the ordering of the V vector is defined by these parameters and
    // the Packed functions in ColorSpinorFieldOrder

    param.nSpin = B[0]->Nspin(); // spin has direct mapping
    param.nColor = B[0]->Ncolor()*Nvec; // nColor = number of colors * number of vectors
    param.create = QUDA_ZERO_FIELD_CREATE;
    // the V field is defined on all sites regardless of B field (maybe the B fields are always full?)
    if (param.siteSubset == QUDA_PARITY_SITE_SUBSET) {
      //keep it the same for staggered:
      param.siteSubset = QUDA_FULL_SITE_SUBSET;
      param.x[0] *= 2;
    }

    if (param.nSpin == 1){
      //param.fieldOrder = QUDA_FLOAT2_FIELD_ORDER;//host orthogonalization is implemented for SPIN_COLOR order only!
      printfQuda("Transfer: creating V field with location %d\n", param.location);
    }
    else{
      printfQuda("Transfer: creating V field with basis %d with location %d\n", param.gammaBasis, param.location);    
    }

    // for cpu transfer this is the V field, for gpu it's just a temporary until we port the block orthogonalization
    V_h = ColorSpinorField::Create(param);

    param.location = QUDA_CUDA_FIELD_LOCATION;
    param.fieldOrder = QUDA_FLOAT2_FIELD_ORDER;//ok for staggered

    V_d = enable_gpu ? ColorSpinorField::Create(param) : 0;

    printfQuda("Transfer: filling V field with zero\n");
    fillV(*V_h); // copy the null space vectors into V

    param = ColorSpinorParam(*B[0]);

    // used for cpu<->gpu transfers
    param.create = QUDA_NULL_FIELD_CREATE;
    fine_tmp_h = ColorSpinorField::Create(param);

    // useful to have around
    coarse_tmp_h = fine_tmp_h->CreateCoarse(geo_bs, spin_bs, Nvec);

    // create temporaries we use to enable us to change basis and for cpu<->gpu transfers
    if (enable_gpu) {
      param = ColorSpinorParam(*B[0]);
      param.location = QUDA_CUDA_FIELD_LOCATION;
      param.fieldOrder = QUDA_FLOAT2_FIELD_ORDER;
      param.create = QUDA_NULL_FIELD_CREATE;
      fine_tmp_d = ColorSpinorField::Create(param);

      // used for basis changing
      coarse_tmp_d = fine_tmp_d->CreateCoarse(geo_bs, spin_bs, Nvec);
    }

    // allocate and compute the fine-to-coarse and coarse-to-fine site maps
    fine_to_coarse_h = static_cast<int*>(safe_malloc(B[0]->Volume()*sizeof(int)));
    coarse_to_fine_h = static_cast<int*>(safe_malloc(B[0]->Volume()*sizeof(int)));

    if (enable_gpu) {
      fine_to_coarse_d = static_cast<int*>(device_malloc(B[0]->Volume()*sizeof(int)));
      coarse_to_fine_d = static_cast<int*>(device_malloc(B[0]->Volume()*sizeof(int)));
    }

    createGeoMap(geo_bs);

    // allocate the fine-to-coarse spin map (underfined for the top level staggered.)
    if (param.nSpin != 1){
      spin_map = static_cast<int*>(safe_malloc(B[0]->Nspin()*sizeof(int)));
      createSpinMap(spin_bs);
    }

    // orthogonalize the blocks
    printfQuda("Transfer: block orthogonalizing\n");
    BlockOrthogonalize(*V_h, Nvec, geo_bs, fine_to_coarse_h, spin_bs);
    printfQuda("Transfer: V block orthonormal check %g\n", blas::norm2(*V_h));
    //for (int x=0; x<Vh->Volume(); x++) static_cast<cpuColorSpinorField*>(Vh)->PrintVector(x);
    //printfQuda("Vh->Volume() = %d Vh->Nspin() = %d Vh->Ncolor = %d Vh->Length() = %d\n", Vh->Volume(), Vh->Nspin(), Vh->Ncolor(), Vh->Length());

    if (enable_gpu) {
      *V_d = *V_h;
      printfQuda("Transferred prolongator to GPU\n");
    }
  }

  Transfer::~Transfer() {
    if (spin_map) host_free(spin_map);
    if (coarse_to_fine_d) device_free(coarse_to_fine_d);
    if (fine_to_coarse_d) device_free(fine_to_coarse_d);
    if (coarse_to_fine_h) host_free(coarse_to_fine_h);
    if (fine_to_coarse_h) host_free(fine_to_coarse_h);
    if (V_h) delete V_h;
    if (V_d) delete V_d;

    if (fine_tmp_h) delete fine_tmp_h;
    if (fine_tmp_d) delete fine_tmp_d;

    if (coarse_tmp_h) delete coarse_tmp_h;
    if (coarse_tmp_d) delete coarse_tmp_d;
  }

  void Transfer::fillV(ColorSpinorField &V) { 
    FillV(V, B, Nvec);  //printfQuda("V fill check %e\n", norm2(*V));
  }

  struct Int2 {
    int x, y;
    Int2() : x(0), y(0) { } 
    Int2(int x, int y) : x(x), y(y) { } 
    
    bool operator<(const Int2 &a) const {
      return (x < a.x) ? true : (x==a.x && y<a.y) ? true : false;
    }
  };

  // compute the fine-to-coarse site map
  void Transfer::createGeoMap(int *geo_bs) {

    int x[QUDA_MAX_DIM];

    ColorSpinorField &fine(*fine_tmp_h);
    ColorSpinorField &coarse(*coarse_tmp_h);

    // compute the coarse grid point for every site (assuming parity ordering currently)
    for (int i=0; i<fine.Volume(); i++) {
      // compute the lattice-site index for this offset index
      fine.LatticeIndex(x, i);
      
      //printfQuda("fine idx %d = fine (%d,%d,%d,%d), ", i, x[0], x[1], x[2], x[3]);

      // compute the corresponding coarse-grid index given the block size
      for (int d=0; d<fine.Ndim(); d++) x[d] /= geo_bs[d];

      // compute the coarse-offset index and store in fine_to_coarse
      int k;
      coarse.OffsetIndex(k, x); // this index is parity ordered
      fine_to_coarse_h[i] = k;

      //printfQuda("coarse after (%d,%d,%d,%d), coarse idx %d\n", x[0], x[1], x[2], x[3], k);
    }

    // now create an inverse-like variant of this

    std::vector<Int2> geo_sort(B[0]->Volume());
    for (unsigned int i=0; i<geo_sort.size(); i++) geo_sort[i] = Int2(fine_to_coarse_h[i], i);
    std::sort(geo_sort.begin(), geo_sort.end());
    for (unsigned int i=0; i<geo_sort.size(); i++) coarse_to_fine_h[i] = geo_sort[i].y;

    if (enable_gpu) {
      cudaMemcpy(fine_to_coarse_d, fine_to_coarse_h, B[0]->Volume()*sizeof(int), cudaMemcpyHostToDevice);
      cudaMemcpy(coarse_to_fine_d, coarse_to_fine_h, B[0]->Volume()*sizeof(int), cudaMemcpyHostToDevice);
      checkCudaError();
    }

  }

  // compute the fine spin to coarse spin map
  void Transfer::createSpinMap(int spin_bs) {

    if(!spin_map) errorQuda("\nSpin map is underfined (was not allocated).\n");

    for (int s=0; s<B[0]->Nspin(); s++) {
      spin_map[s] = s / spin_bs;
    }

  }

  // apply the prolongator
  void Transfer::P(ColorSpinorField &out, const ColorSpinorField &in) const {

    ColorSpinorField *input = const_cast<ColorSpinorField*>(&in);
    ColorSpinorField *output = &out;
    const ColorSpinorField *V = use_gpu ? V_d : V_h;
    const int *fine_to_coarse = use_gpu ? fine_to_coarse_d : fine_to_coarse_h;

    if (use_gpu) {
      if (in.Location() == QUDA_CPU_FIELD_LOCATION) input = coarse_tmp_d;
      if (out.Location() == QUDA_CPU_FIELD_LOCATION ||
	  out.GammaBasis() != V->GammaBasis()) output = fine_tmp_d;
      if (!enable_gpu) errorQuda("not created with enable_gpu set, so cannot run on GPU");
    } else {
      output = (out.Location() == QUDA_CUDA_FIELD_LOCATION) ? fine_tmp_h : &out;
    }

    *input = in; // copy result to input field (aliasing handled automatically)
    
    if ((V->Nspin() != 1) && ((output->GammaBasis() != V->GammaBasis()) || (input->GammaBasis() != V->GammaBasis()))){
      errorQuda("Cannot apply prolongator using fields in a different basis from the null space (%d,%d) != %d",
		output->GammaBasis(), in.GammaBasis(), V->GammaBasis());
    }

    Prolongate(*output, *input, *V, Nvec, fine_to_coarse, spin_map);

    out = *output; // copy result to out field (aliasing handled automatically)
  }

  // apply the restrictor
  void Transfer::R(ColorSpinorField &out, const ColorSpinorField &in) const {

    ColorSpinorField *input = &const_cast<ColorSpinorField&>(in);
    ColorSpinorField *output = &out;
    const ColorSpinorField *V = use_gpu ? V_d : V_h;
    const int *fine_to_coarse = use_gpu ? fine_to_coarse_d : fine_to_coarse_h;
    const int *coarse_to_fine = use_gpu ? coarse_to_fine_d : coarse_to_fine_h;

    if (use_gpu) {
      if (out.Location() == QUDA_CPU_FIELD_LOCATION) output = coarse_tmp_d;
      if (in.Location() == QUDA_CPU_FIELD_LOCATION ||
	  in.GammaBasis() != V->GammaBasis()) input = fine_tmp_d;
      if (!enable_gpu) errorQuda("not created with enable_gpu set, so cannot run on GPU");
    } else {
      if (in.Location() == QUDA_CUDA_FIELD_LOCATION) input = fine_tmp_h;
    }

    *input = in; // copy result to input field (aliasing handled automatically)  
    
    if ( V->Nspin() != 1 && ( output->GammaBasis() != V->GammaBasis() || input->GammaBasis() != V->GammaBasis() ) )
      errorQuda("Cannot apply restrictor using fields in a different basis from the null space (%d,%d) != %d",
		out.GammaBasis(), input->GammaBasis(), V->GammaBasis());

    Restrict(*output, *input, *V, Nvec, fine_to_coarse, coarse_to_fine, spin_map);

    out = *output; // copy result to out field (aliasing handled automatically)

    // only need to synchronize if we're transferring from GPU to CPU
    if (out.Location() == QUDA_CPU_FIELD_LOCATION && in.Location() == QUDA_CUDA_FIELD_LOCATION)
      cudaDeviceSynchronize();
  }

} // namespace quda
