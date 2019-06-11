#include <transfer.h>
#include <color_spinor_field.h>
#include <gauge_field.h>

#ifdef GPU_MULTIGRID 
#include <staggered_coarse_op.cuh>
#endif

namespace quda {

#ifdef GPU_MULTIGRID

  template <typename Float, typename vFloat, int fineColor, int fineSpin, int coarseColor, int coarseSpin>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, GaugeField &Yatomic, GaugeField &Xatomic,
      const Transfer &T, const GaugeField &g, double mass, QudaDiracType dirac, QudaMatPCType matpc) {

    QudaFieldLocation location = Y.Location();

    if (location == QUDA_CPU_FIELD_LOCATION) {

      constexpr QudaFieldOrder csOrder = QUDA_SPACE_SPIN_COLOR_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_QDP_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      typedef typename gauge::FieldOrder<Float,fineColor,1,gOrder> gFine;
      typedef typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,vFloat> gCoarse;
      typedef typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,storeType> gCoarseAtomic;

      gFine gAccessor(const_cast<GaugeField&>(g));
      gCoarse yAccessor(const_cast<GaugeField&>(Y));
      gCoarse xAccessor(const_cast<GaugeField&>(X));
      gCoarseAtomic yAccessorAtomic(const_cast<GaugeField&>(Yatomic));
      gCoarseAtomic xAccessorAtomic(const_cast<GaugeField&>(Xatomic));

      calculateStaggeredY<Float,fineSpin,fineColor,coarseSpin,coarseColor>
        (yAccessor, xAccessor, yAccessorAtomic, xAccessorAtomic, 
         gAccessor, Y, X, Yatomic, Xatomic, g, mass, dirac, matpc,
         T.fineToCoarse(location), T.coarseToFine(location));

    } else {

      constexpr QudaFieldOrder csOrder = QUDA_FLOAT2_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_FLOAT2_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      constexpr bool use_tex = __COMPUTE_CAPABILITY__ < 520 ? true : false; // on pre-Maxwell-2 use textures/ldg to get caching
      typedef typename gauge::FieldOrder<Float,fineColor,1,gOrder,true,Float,use_tex> gFine;
      typedef typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,vFloat> gCoarse;
      typedef typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,storeType> gCoarseAtomic;

      gFine gAccessor(const_cast<GaugeField&>(g));
      gCoarse yAccessor(const_cast<GaugeField&>(Y));
      gCoarse xAccessor(const_cast<GaugeField&>(X));
      gCoarseAtomic yAccessorAtomic(const_cast<GaugeField&>(Yatomic));
      gCoarseAtomic xAccessorAtomic(const_cast<GaugeField&>(Xatomic));

      calculateStaggeredY<Float,fineSpin,fineColor,coarseSpin,coarseColor>
        (yAccessor, xAccessor, yAccessorAtomic, xAccessorAtomic,
         gAccessor, Y, X, Yatomic, Xatomic, g, mass, dirac, matpc,
         T.fineToCoarse(location), T.coarseToFine(location));

    }

  }

  // template on the number of coarse degrees of freedom
  template <typename Float, typename vFloat, int fineColor, int fineSpin>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, GaugeField &Yatomic, GaugeField &Xatomic,
      const Transfer &T, const GaugeField &g, double mass, QudaDiracType dirac, QudaMatPCType matpc) {
    const int coarseSpin = 2;
    const int coarseColor = Y.Ncolor() / coarseSpin;

#if 0
    if (coarseColor == 4) {
      calculateY<Float,vFloat,fineColor,fineSpin,4,coarseSpin>(Y, X, Yatomic, Xatomic, T, g, c, mass dirac, matpc);
#endif
    if (coarseColor == 24) { // free field staggered
      calculateStaggeredY<Float,vFloat,fineColor,fineSpin,24,coarseSpin>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
    } else {
      errorQuda("Unsupported number of coarse dof %d\n", Y.Ncolor());
    }
  }

  // template on fine spin
  template <typename Float, typename vFloat, int fineColor>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, GaugeField &Yatomic, GaugeField &Xatomic, 
      const Transfer &T, const GaugeField &g, double mass, QudaDiracType dirac, QudaMatPCType matpc) {
    if (T.Vectors().Nspin() == 1) {
      calculateStaggeredY<Float,vFloat,fineColor,1>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
    } else {
      errorQuda("Unsupported number of spins %d\n", T.Vectors(X.Location()).Nspin());
    }
  }

  // template on fine colors
  template <typename Float, typename vFloat>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, GaugeField &Yatomic, GaugeField &Xatomic,
      const Transfer &T, const GaugeField &g, double mass, QudaDiracType dirac, QudaMatPCType matpc) {
    if (g.Ncolor() == 3) {
      calculateStaggeredY<Float,vFloat,3>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
    } else {
      errorQuda("Unsupported number of colors %d\n", g.Ncolor());
    }
  }

  //Does the heavy lifting of creating the coarse color matrices Y
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, GaugeField &Yatomic, GaugeField &Xatomic,
      const Transfer &T, const GaugeField &g, double mass, QudaDiracType dirac, QudaMatPCType matpc) {
    checkPrecision(Xatomic, Yatomic, g);
    checkPrecision(T.Vectors(X.Location()), X, Y);

    printfQuda("Computing Y field......\n");

    if (Y.Precision() == QUDA_DOUBLE_PRECISION) {
#ifdef GPU_MULTIGRID_DOUBLE
      if (T.Vectors(X.Location()).Precision() == QUDA_DOUBLE_PRECISION) {
        calculateStaggeredY<double,double>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", Y.Precision());
      }
#else
      errorQuda("Double precision multigrid has not been enabled");
#endif
    } else if (Y.Precision() == QUDA_SINGLE_PRECISION) {
      if (T.Vectors(X.Location()).Precision() == QUDA_SINGLE_PRECISION) {
        calculateStaggeredY<float,float>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", T.Vectors(X.Location()).Precision());
      }
    } else if (Y.Precision() == QUDA_HALF_PRECISION) {
      if (T.Vectors(X.Location()).Precision() == QUDA_HALF_PRECISION) {
        calculateStaggeredY<float,short>(Y, X, Yatomic, Xatomic, T, g, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", T.Vectors(X.Location()).Precision());
      }
    } else {
      errorQuda("Unsupported precision %d\n", Y.Precision());
    }
    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("....done computing Y field\n");
  }

#endif // GPU_MULTIGRID

  //Calculates the coarse color matrix and puts the result in Y.
  //N.B. Assumes Y, X have been allocated.
  void StaggeredCoarseOp(GaugeField &Y, GaugeField &X, const Transfer &T,
		const cudaGaugeField &gauge,
		double mass, QudaDiracType dirac, QudaMatPCType matpc) {

#ifdef GPU_MULTIGRID

    QudaPrecision precision = Y.Precision();
    QudaFieldLocation location = checkLocation(Y, X);

    GaugeField *U = location == QUDA_CUDA_FIELD_LOCATION ? const_cast<cudaGaugeField*>(&gauge) : nullptr;

    if (location == QUDA_CPU_FIELD_LOCATION) {
      //First make a cpu gauge field from the cuda gauge field
      int pad = 0;
      GaugeFieldParam gf_param(gauge.X(), precision, QUDA_RECONSTRUCT_NO, pad, gauge.Geometry());
      gf_param.order = QUDA_QDP_GAUGE_ORDER;
      gf_param.fixed = gauge.GaugeFixed();
      gf_param.link_type = gauge.LinkType();
      gf_param.t_boundary = gauge.TBoundary();
      gf_param.anisotropy = gauge.Anisotropy();
      gf_param.gauge = NULL;
      gf_param.create = QUDA_NULL_FIELD_CREATE;
      gf_param.siteSubset = QUDA_FULL_SITE_SUBSET;
      gf_param.nFace = 1;
      gf_param.ghostExchange = QUDA_GHOST_EXCHANGE_PAD;

      U = new cpuGaugeField(gf_param);

      //Copy the cuda gauge field to the cpu
      gauge.saveCPUField(*static_cast<cpuGaugeField*>(U));
    } else if (location == QUDA_CUDA_FIELD_LOCATION && gauge.Reconstruct() != QUDA_RECONSTRUCT_NO) {
      //Create a copy of the gauge field with no reconstruction, required for fine-grained access
      GaugeFieldParam gf_param(gauge);
      gf_param.reconstruct = QUDA_RECONSTRUCT_NO;
      gf_param.order = QUDA_FLOAT2_GAUGE_ORDER;
      gf_param.setPrecision(gf_param.Precision());
      U = new cudaGaugeField(gf_param);

      U->copy(gauge);
    }

    GaugeField *Yatomic = &Y;
    GaugeField *Xatomic = &X;
    if (Y.Precision() < QUDA_SINGLE_PRECISION) {
      // we need to coarsen into single precision fields (float or int), so we allocate temporaries for this purpose
      // else we can just coarsen directly into the original fields
      GaugeFieldParam param(X); // use X since we want scalar geometry
      param.location = location;
      param.setPrecision(QUDA_SINGLE_PRECISION, location == QUDA_CUDA_FIELD_LOCATION ? true : false);

      Yatomic = GaugeField::Create(param);
      Xatomic = GaugeField::Create(param);
    }

    calculateStaggeredY(Y, X, *Yatomic, *Xatomic, T, *U, mass, dirac, matpc);

    if (Yatomic != &Y) delete Yatomic;
    if (Xatomic != &X) delete Xatomic;

    if (U != &gauge) delete U;
#else
    errorQuda("Multigrid has not been built");
#endif
  }

} //namespace quda
