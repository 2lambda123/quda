#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quda.h>
#include <quda_internal.h>
//#include <color_spinor_field.h>
//#include <dirac_quda.h>
//#include <dslash_quda.h>
//#include <invert_quda.h>
//#include <util_quda.h>
//#include <blas_quda.h>


//#include <host_utils.h>
//#include <misc.h>

#include <stoch_laph_quark_smear_kernels.h>

using namespace quda;

//!< Profiler for laphInvertSourcesQuda
static TimeProfile profileLaphInvert("laphInvertSourcesQuda");


void laphSinkProject(void *host_quark, void *host_evec, void *host_sinks,
		     QudaInvertParam inv_param, const int X[4], int t_size)
{
  // Parameter object describing the sources and smeared quarks
  ColorSpinorParam cpu_quark_param(host_quark, inv_param, X, false, QUDA_CPU_FIELD_LOCATION);
  cpu_quark_param.gammaBasis = QUDA_DEGRAND_ROSSI_GAMMA_BASIS;
  
  // QUDA style wrapper around the host data
  std::vector<ColorSpinorField*> quark;
  cpu_quark_param.v = host_quark;
  quark.push_back(ColorSpinorField::Create(cpu_quark_param));
  
  // Parameter object describing evecs
  ColorSpinorParam cpu_evec_param(host_evec, inv_param, X, false, QUDA_CPU_FIELD_LOCATION);
  // Switch to spin 1
  cpu_evec_param.nSpin = 1;
  // QUDA style wrapper around the host data
  std::vector<ColorSpinorField*> evec;
  cpu_evec_param.v = host_evec;
  evec.push_back(ColorSpinorField::Create(cpu_evec_param));
  
  // Create device vectors
  ColorSpinorParam cuda_quark_param(cpu_quark_param);
  cuda_quark_param.location = QUDA_CUDA_FIELD_LOCATION;
  cuda_quark_param.create = QUDA_ZERO_FIELD_CREATE;
  cuda_quark_param.setPrecision(inv_param.cpu_prec, inv_param.cpu_prec, true);
  std::vector<ColorSpinorField *> quda_quark;
  quda_quark.push_back(ColorSpinorField::Create(cuda_quark_param));
  // Copy data from host to device
  *quda_quark[0] = *quark[0];

  // Create device vectors for evecs
  ColorSpinorParam cuda_evec_param(cpu_evec_param);
  cuda_evec_param.location = QUDA_CUDA_FIELD_LOCATION;
  cuda_evec_param.create = QUDA_ZERO_FIELD_CREATE;
  cuda_evec_param.setPrecision(inv_param.cpu_prec, inv_param.cpu_prec, true);
  cuda_evec_param.nSpin = 1;
  std::vector<ColorSpinorField *> quda_evec;
  quda_evec.push_back(ColorSpinorField::Create(cuda_evec_param));
  // Copy data from host to device
  *quda_evec[0] = *evec[0];
  
  // We now perfrom the projection onto the eigenspace. The data
  // is placed in host_sinks in i, X, Y, Z, T, spin order
  double time_lsp = -clock();
  //int t_size = comm_dim(3) * t_size;
  Complex sinks[cuda_quark_param.nSpin * t_size];
  evecProjectQuda(*quda_quark[0], *quda_evec[0], t_size, sinks);
  time_lsp += clock();
  saveTuneCache(true);
  
  printfQuda("LSP time = %e\n", time_lsp/CLOCKS_PER_SEC);

  // Clean up memory allocations
  delete quark[0];
  delete quda_quark[0];
  delete evec[0];
  delete quda_evec[0];
}


#if 0

void laphSourceConstruct(std::vector<quda::ColorSpinorField *> &quarks, std::vector<quda::ColorSpinorField *> &evecs,
                         const Complex noise_array[], const int dil_scheme)
{
  int n_dil_vecs = evecs.size() / dil_scheme;
  printfQuda("evecs.size() = %d\n", (int)evecs.size());
  printfQuda("quarks.size() = %d\n", (int)quarks.size());
  printfQuda("dil_scheme = %d\n", dil_scheme);
  printfQuda("n_dil_vecs = %d\n", n_dil_vecs);
  // Construct 4 vectors to hold the 4 spin sources

  ColorSpinorParam csp_evecs(*evecs[0]);
  csp_evecs.create = QUDA_ZERO_FIELD_CREATE;
  std::vector<ColorSpinorField *> sources;
  sources.reserve(4);
  for (int i = 0; i < 4; i++) { sources.push_back(ColorSpinorField::Create(csp_evecs)); }

  // Construct 4 vectors to hold the 4 spin DILUTED sources
  ColorSpinorParam csp_quarks(*quarks[0]);
  // csp_quarks.create = QUDA_ZERO_FIELD_CREATE;
  std::vector<ColorSpinorField *> dil_sources;
  dil_sources.reserve(4);
  for (int i = 0; i < 4; i++) { dil_sources.push_back(ColorSpinorField::Create(csp_quarks)); }

  // Loop over dilutions
  for (int i = 0; i < dil_scheme; i++) {

    // Collect the relevant eigenvectors
    std::vector<ColorSpinorField *> dil_evecs_ptr;
    dil_evecs_ptr.reserve(n_dil_vecs);
    for (int j = 0; j < n_dil_vecs; j++) { dil_evecs_ptr.push_back(evecs[i + j * dil_scheme]); }

    // Collect the relevant noise values
    Complex noise[4 * n_dil_vecs];
    for (int j = 0; j < n_dil_vecs; j++) {
      for (int spin = 0; spin < 4; spin++) { noise[4 * j + spin] = noise_array[4 * (j * dil_scheme + i) + spin]; }
    }

    // Construct source
    blas::caxpy(noise, dil_evecs_ptr, sources);

    for (int spin = 0; spin < 4; spin++) {
      spinDiluteQuda(*dil_sources[spin], *sources[spin], spin);
      // Copy spin diluted sources into quark array
      *quarks[4 * i + spin] = *dil_sources[spin];
    }
  }
  printfQuda("All nSpin * dil_scheme sources constructed\n");

  for (int spin = 0; spin < 4; spin++) {
    delete dil_sources[spin];
    delete sources[spin];
  }
}

void laphSourceInvert(std::vector<ColorSpinorField *> &quarks, QudaInvertParam *inv_param)
{
  bool pc_solve = (inv_param->solve_type == QUDA_DIRECT_PC_SOLVE) || (inv_param->solve_type == QUDA_NORMOP_PC_SOLVE)
    || (inv_param->solve_type == QUDA_NORMERR_PC_SOLVE);

  Dirac *d = nullptr;
  Dirac *dSloppy = nullptr;
  Dirac *dPre = nullptr;

  // create the dirac operator
  createDirac(d, dSloppy, dPre, *inv_param, pc_solve);

  Dirac &dirac = *d;
  Dirac &diracSloppy = *dSloppy;
  Dirac &diracPre = *dPre;

  // `in` will point to the relevant quark[i] source
  // `out` will be copy back to quark[i]
  ColorSpinorField *in = nullptr;
  ColorSpinorField *out = nullptr;
  ColorSpinorField *x = nullptr;
  ColorSpinorField *b = nullptr;

  ColorSpinorParam cuda_param(*quarks[0]);
  b = ColorSpinorField::Create(cuda_param);
  x = ColorSpinorField::Create(cuda_param);

  // Zero solver stats
  inv_param->secs = 0;
  inv_param->gflops = 0;
  inv_param->iter = 0;
  double secs = 0.0;
  double gflops = 0.0;
  int iter = 0;

  for (int i = 0; i < (int)quarks.size(); i++) {
    int t_size = comm_dim(3) * tdim;

    for(int t=0; t<t_size; t++) {
      *b = *quarks[i];
      //temporalDiluteQuda(*b, t);
      dirac.prepare(in, out, *x, *b, inv_param->solution_type);
      DiracM m(dirac), mSloppy(diracSloppy), mPre(diracPre);
      SolverParam solverParam(*inv_param);
      Solver *solve = Solver::create(solverParam, m, mSloppy, mPre, profileLaphInvert);
      
      (*solve)(*out, *in);
      
      *quarks[i] = *x;
      solverParam.updateInvertParam(*inv_param);
      delete solve;
    }
    
    // Accumulate Solver stats
    secs += inv_param->secs;
    gflops += inv_param->gflops;
    iter += inv_param->iter;
    // Zero solver stats
    inv_param->secs = 0;
    inv_param->gflops = 0;
    inv_param->iter = 0;

  }

  delete x;
  delete b;
  delete d;
  delete dSloppy;
  delete dPre;

  
  //for(int j=0; j<V; j++) quarks[0]->PrintVector(j);
}

void stochLaphSmearQuda(void **host_quarks, void **host_evecs,
			void *host_noise, void *host_sinks,
			const int dil_scheme, const int n_evecs, 
			QudaInvertParam inv_param, const int X[4])
{
  int n_sources = 4 * dil_scheme;
  
  // Parameter object describing the sources and smeared quarks
  ColorSpinorParam cpu_quark_param(host_quarks[0], inv_param, X, false, QUDA_CPU_FIELD_LOCATION);
  cpu_quark_param.gammaBasis = QUDA_UKQCD_GAMMA_BASIS;
  // QUDA style wrappers around the host data
  std::vector<ColorSpinorField*> quarks;
  quarks.reserve(n_sources);
  for (int i = 0; i < n_sources; i++) {
    cpu_quark_param.v = host_quarks[i];
    quarks.push_back(ColorSpinorField::Create(cpu_quark_param));
  }  
  
  // Host side data for eigenvecs
  // Parameter object describing evecs
  ColorSpinorParam cpu_evec_param(host_evecs[0], inv_param, X, false, QUDA_CPU_FIELD_LOCATION);
  // Switch to spin 1
  cpu_evec_param.nSpin = 1;
  // QUDA style wrappers around the host data
  std::vector<ColorSpinorField*> evecs;
  evecs.reserve(n_evecs);
  for (int i = 0; i < n_evecs; i++) {
    cpu_evec_param.v = host_evecs[i];
    evecs.push_back(ColorSpinorField::Create(cpu_evec_param));
  }  

  // Create device vectors for quarks
  ColorSpinorParam cuda_quark_param(cpu_quark_param);
  cuda_quark_param.location = QUDA_CUDA_FIELD_LOCATION;
  cuda_quark_param.create = QUDA_ZERO_FIELD_CREATE;
  cuda_quark_param.setPrecision(inv_param.cpu_prec, inv_param.cpu_prec, true);
  std::vector<ColorSpinorField *> quda_quarks;
  quda_quarks.reserve(n_sources);
  for (int i = 0; i < n_sources; i++) {
    quda_quarks.push_back(ColorSpinorField::Create(cuda_quark_param));
    // Copy data from host to device
    *quda_quarks[i] = *quarks[i];
  }

  // Create device vectors for evecs
  ColorSpinorParam cuda_evec_param(cpu_evec_param);
  cuda_evec_param.location = QUDA_CUDA_FIELD_LOCATION;
  cuda_evec_param.create = QUDA_ZERO_FIELD_CREATE;
  cuda_evec_param.setPrecision(inv_param.cpu_prec, inv_param.cpu_prec, true);
  cuda_evec_param.nSpin = 1;
  std::vector<ColorSpinorField *> quda_evecs;
  quda_evecs.reserve(n_evecs);
  for (int i = 0; i < n_evecs; i++) {
    quda_evecs.push_back(ColorSpinorField::Create(cuda_evec_param));
    // Copy data from host to device
    *quda_evecs[i] = *evecs[i];
  }

  // Recast the noise as complex double
  Complex *host_noise_ = &((Complex*)host_noise)[0];
  // Use the dilution scheme and stochstic noise to construct quark sources
  double time_lsc = -clock();
  laphSourceConstruct(quda_quarks, quda_evecs, host_noise_, dil_scheme);
  time_lsc += clock();
  saveTuneCache(true);
  
  // The quarks sources are located in quda_quarks. We invert using those
  // sources and place the propagator from that solve back into quda_quarks
  double time_lsi = -clock();
  laphSourceInvert(quda_quarks, &inv_param);
  time_lsi += clock();
  saveTuneCache(true);

  // We now perfrom the projection back onto the eigenspace. The data
  // is placed in host_sinks in i, X, Y, Z, T, spin order
  double time_lsp = -clock();
  time_lsp += clock();
  saveTuneCache(true);
  
  printfQuda("LSC time = %e\n", time_lsc/CLOCKS_PER_SEC);
  printfQuda("LSI time = %e\n", time_lsi/CLOCKS_PER_SEC);
  printfQuda("LSP time = %e\n", time_lsp/CLOCKS_PER_SEC);

  // Clean up memory allocations
  for (int i = 0; i < n_sources; i++) {
    delete quarks[i];
    delete quda_quarks[i];
  }

  for (int i = 0; i < n_evecs; i++) {
    delete evecs[i];
    delete quda_evecs[i];
  }  
}
#endif