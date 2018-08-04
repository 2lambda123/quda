// *** CUDA DSLASH DAGGER ***

#define DSLASH_SHARED_FLOATS_PER_THREAD 0

// This is for tensor core ONLY so assumming CUDA_VERSION >= 9000

// input spinor needs to be half
#define POW(a, b) __fast_pow(a, b)
#define i00_re I0.x
#define i00_im I0.y
#define i01_re I0.z
#define i01_im I0.w
#define i02_re I1.x
#define i02_im I1.y
#define i10_re I1.z
#define i10_im I1.w
#define i11_re I2.x
#define i11_im I2.y
#define i12_re I2.z
#define i12_im I2.w
#define i20_re I3.x
#define i20_im I3.y
#define i21_re I3.z
#define i21_im I3.w
#define i22_re I4.x
#define i22_im I4.y
#define i30_re I4.z
#define i30_im I4.w
#define i31_re I5.x
#define i31_im I5.y
#define i32_re I5.z
#define i32_im I5.w
#define m5 param.m5_f
#define mdwf_b5 param.mdwf_b5_f
#define mdwf_c5 param.mdwf_c5_f
#define mferm param.mferm_f
#define a param.a
#define b param.b

// output spinor
float o00_re = 0.0f;
float o00_im = 0.0f;
float o01_re = 0.0f;
float o01_im = 0.0f;
float o02_re = 0.0f;
float o02_im = 0.0f;
float o10_re = 0.0f;
float o10_im = 0.0f;
float o11_re = 0.0f;
float o11_im = 0.0f;
float o12_re = 0.0f;
float o12_im = 0.0f;
float o20_re = 0.0f;
float o20_im = 0.0f;
float o21_re = 0.0f;
float o21_im = 0.0f;
float o22_re = 0.0f;
float o22_im = 0.0f;
float o30_re = 0.0f;
float o30_im = 0.0f;
float o31_re = 0.0f;
float o31_im = 0.0f;
float o32_re = 0.0f;
float o32_im = 0.0f;

MDWFSharedMemory<float4> sm_data;

half*  sm_b = (half*)((void*)sm_data);
float* sm_c = (float*)(sm_b + param.dc.Ls*24*blockDim.x);
half*  sm_a = (half*)(sm_c + param.dc.Ls*24*blockDim.x);

#define SHARED_STRIDE 32 // to avoid bank conflicts on Fermi

#include "io_spinor.h"

bool idle = false;
int sid = (blockIdx.y*blockDim.y + threadIdx.y)*param.threads + blockIdx.x*blockDim.x+threadIdx.x;
if (blockIdx.x*blockDim.x+threadIdx.x >= param.threads){
	idle = true;
}

int X, coord[5], boundaryCrossing;

if(!idle){

  if( param.partial_length ){
    coordsFromIndexShrinked<5,QUDA_4D_PC,EVEN_X>(X, coord, sid, param);
  }else{
  
    boundaryCrossing = sid/param.dc.Xh[0] + sid/(param.dc.X[1]*param.dc.Xh[0]) + sid/(param.dc.X[2]*param.dc.X[1]*param.dc.Xh[0]);
  
    X = 2*sid + (boundaryCrossing + param.parity) % 2;
    coord[4] = X/(param.dc.X[0]*param.dc.X[1]*param.dc.X[2]*param.dc.X[3]);
  
  }

//  boundaryCrossing = sid/param.dc.Xh[0] + sid/(param.dc.X[1]*param.dc.Xh[0]) + sid/(param.dc.X[2]*param.dc.X[1]*param.dc.Xh[0]);
//  
//  X = 2*sid + (boundaryCrossing + param.parity) % 2;
//  coord[4] = X/(param.dc.X[0]*param.dc.X[1]*param.dc.X[2]*param.dc.X[3]);

READ_SPINOR( SPINORTEX, param.sp_stride, X/2, X/2 );

// data layout for tensor core B and C: spatial, color, complex, spin, s; Lsx4 by Lsx4 @ Lsx4 by 6xblockDim.x.
// lda = Lsx4, column-major
// ldb = Lsx4, column-major
// total number of halves = Ls*24*blockDim.x
sm_b[ (((threadIdx.x*3+0)*2+0)*4+0)*param.dc.Ls+coord[4] ] = i00_re;
sm_b[ (((threadIdx.x*3+0)*2+1)*4+0)*param.dc.Ls+coord[4] ] = i00_im;
sm_b[ (((threadIdx.x*3+1)*2+0)*4+0)*param.dc.Ls+coord[4] ] = i01_re;
sm_b[ (((threadIdx.x*3+1)*2+1)*4+0)*param.dc.Ls+coord[4] ] = i01_im;
sm_b[ (((threadIdx.x*3+2)*2+0)*4+0)*param.dc.Ls+coord[4] ] = i02_re;
sm_b[ (((threadIdx.x*3+2)*2+1)*4+0)*param.dc.Ls+coord[4] ] = i02_im;
sm_b[ (((threadIdx.x*3+0)*2+0)*4+1)*param.dc.Ls+coord[4] ] = i10_re;
sm_b[ (((threadIdx.x*3+0)*2+1)*4+1)*param.dc.Ls+coord[4] ] = i10_im;
sm_b[ (((threadIdx.x*3+1)*2+0)*4+1)*param.dc.Ls+coord[4] ] = i11_re;
sm_b[ (((threadIdx.x*3+1)*2+1)*4+1)*param.dc.Ls+coord[4] ] = i11_im;
sm_b[ (((threadIdx.x*3+2)*2+0)*4+1)*param.dc.Ls+coord[4] ] = i12_re;
sm_b[ (((threadIdx.x*3+2)*2+1)*4+1)*param.dc.Ls+coord[4] ] = i12_im;
sm_b[ (((threadIdx.x*3+0)*2+0)*4+2)*param.dc.Ls+coord[4] ] = i20_re;
sm_b[ (((threadIdx.x*3+0)*2+1)*4+2)*param.dc.Ls+coord[4] ] = i20_im;
sm_b[ (((threadIdx.x*3+1)*2+0)*4+2)*param.dc.Ls+coord[4] ] = i21_re;
sm_b[ (((threadIdx.x*3+1)*2+1)*4+2)*param.dc.Ls+coord[4] ] = i21_im;
sm_b[ (((threadIdx.x*3+2)*2+0)*4+2)*param.dc.Ls+coord[4] ] = i22_re;
sm_b[ (((threadIdx.x*3+2)*2+1)*4+2)*param.dc.Ls+coord[4] ] = i22_im;
sm_b[ (((threadIdx.x*3+0)*2+0)*4+3)*param.dc.Ls+coord[4] ] = i30_re;
sm_b[ (((threadIdx.x*3+0)*2+1)*4+3)*param.dc.Ls+coord[4] ] = i30_im;
sm_b[ (((threadIdx.x*3+1)*2+0)*4+3)*param.dc.Ls+coord[4] ] = i31_re;
sm_b[ (((threadIdx.x*3+1)*2+1)*4+3)*param.dc.Ls+coord[4] ] = i31_im;
sm_b[ (((threadIdx.x*3+2)*2+0)*4+3)*param.dc.Ls+coord[4] ] = i32_re;
sm_b[ (((threadIdx.x*3+2)*2+1)*4+3)*param.dc.Ls+coord[4] ] = i32_im;

// Construct matrix A: TODO: should be careful about the idle threads.

// threadIdx.x==0 should not be idle(?).
if(threadIdx.x == 0){

#ifdef MDWF_mode   // Check whether MDWF option is enabled
    half kappa = -(static_cast<half>(mdwf_c5[ coord[4] ])*(static_cast<half>(4.0) + static_cast<half>(m5)) - static_cast<half>(1.0))/(static_cast<half>(mdwf_b5[ coord[4] ])*(static_cast<half>(4.0) + static_cast<half>(m5)) + static_cast<half>(1.0));
#else
    half kappa = static_cast<half>(2.0)*static_cast<half>(a);
#endif  // select MDWF mode
  
  half inv_d_n = static_cast<half>(0.5) / ( static_cast<half>(1.0) + static_cast<half>(POW(kappa,param.dc.Ls))*static_cast<half>(mferm) );
  half factorR;
  half factorL;

  for(int s  = 0; s  < param.dc.Ls; s++){
  
    int exponent = coord[4]  > s ? param.dc.Ls-coord[4]+s : s-coord[4];
    factorR = inv_d_n * static_cast<half>(POW(kappa,exponent))  * ( coord[4] > s ? static_cast<half>(-mferm) : static_cast<half>(1.0) );
    
    int exponent2 = coord[4] < s ? param.dc.Ls-s+coord[4] : coord[4]-s;
    factorL = inv_d_n * static_cast<half>(POW(kappa,exponent2)) * ( coord[4] < s ? static_cast<half>(-mferm) : static_cast<half>(1.0) );

    // (mu, s) by (nu, t). column-major. t := threadIdx.y
    sm_a[ (0*param.dc.Ls+s)*(param.dc.Ls*4)+(0*param.dc.Ls+coord[4]) ] = factorR + factorL;
    sm_a[ (0*param.dc.Ls+s)*(param.dc.Ls*4)+(1*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (0*param.dc.Ls+s)*(param.dc.Ls*4)+(2*param.dc.Ls+coord[4]) ] = factorR - factorL;
    sm_a[ (0*param.dc.Ls+s)*(param.dc.Ls*4)+(3*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    
    sm_a[ (1*param.dc.Ls+s)*(param.dc.Ls*4)+(0*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (1*param.dc.Ls+s)*(param.dc.Ls*4)+(1*param.dc.Ls+coord[4]) ] = factorR + factorL;
    sm_a[ (1*param.dc.Ls+s)*(param.dc.Ls*4)+(2*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (1*param.dc.Ls+s)*(param.dc.Ls*4)+(3*param.dc.Ls+coord[4]) ] = factorR - factorL;
    
    sm_a[ (2*param.dc.Ls+s)*(param.dc.Ls*4)+(0*param.dc.Ls+coord[4]) ] = factorR - factorL;
    sm_a[ (2*param.dc.Ls+s)*(param.dc.Ls*4)+(1*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (2*param.dc.Ls+s)*(param.dc.Ls*4)+(2*param.dc.Ls+coord[4]) ] = factorR + factorL;
    sm_a[ (2*param.dc.Ls+s)*(param.dc.Ls*4)+(3*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    
    sm_a[ (3*param.dc.Ls+s)*(param.dc.Ls*4)+(0*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (3*param.dc.Ls+s)*(param.dc.Ls*4)+(1*param.dc.Ls+coord[4]) ] = factorR - factorL;
    sm_a[ (3*param.dc.Ls+s)*(param.dc.Ls*4)+(2*param.dc.Ls+coord[4]) ] = static_cast<half>(0.0f);
    sm_a[ (3*param.dc.Ls+s)*(param.dc.Ls*4)+(3*param.dc.Ls+coord[4]) ] = factorR + factorL;
  }
}

}__syncthreads();

// wmma.h
{
using namespace nvcuda;

const int WMMA_M = 16;
const int WMMA_N = 16;
const int WMMA_K = 16;

const int M = param.dc.Ls*4;
const int N = 6*blockDim.x;
const int K = param.dc.Ls*4;

int tm_dim = M / WMMA_M;
int tn_dim = N / WMMA_N;

// The actual/physical warp assigned to each thread in this block
int phys_warp_n_dim = blockDim.x/warpSize; // TODO: should make sure blockDim.x is AT LEAST 32.
int phys_warp_m_dim = blockDim.y;

int phys_warp_n = threadIdx.x/warpSize;
int phys_warp_m = threadIdx.y; 

int total_num_warp = phys_warp_n_dim*phys_warp_m_dim;
int total_num_tile = tm_dim*tn_dim;

int warp_cycle = total_num_tile/total_num_warp;

// Set up the wmma stuff
wmma::fragment<wmma::matrix_a, WMMA_M, WMMA_N, WMMA_K, half, wmma::col_major> a_frag;
wmma::fragment<wmma::matrix_b, WMMA_M, WMMA_N, WMMA_K, half, wmma::col_major> b_frag;
wmma::fragment<wmma::accumulator, WMMA_M, WMMA_N, WMMA_K, float> c_frag;
// Zero the initial acc.

for(int c = 0; c < warp_cycle; c++){
  int phys_warp_index = phys_warp_n*phys_warp_m_dim+phys_warp_m + total_num_warp*c;
  // The logical warp assigned to each part of the matrix.
  int warp_n = phys_warp_index / tm_dim;
  int warp_m = phys_warp_index % tm_dim;

  wmma::fill_fragment(c_frag, 0.0f);
  for( int k = 0; k < K; k+=WMMA_K ){
    int a_row = warp_m*WMMA_M;
    int a_col = k;

    int b_row = k;
    int b_col = warp_n*WMMA_N;

    if(a_row < M && a_col < K && b_row < K && b_col < N) {    
      // Load Matrix
      wmma::load_matrix_sync(a_frag, sm_a+a_row+a_col*M, M);
      wmma::load_matrix_sync(b_frag, sm_b+b_row+b_col*K, K);
      // Perform the matrix multiplication
      wmma::mma_sync(c_frag, a_frag, b_frag, c_frag);
    }
    __syncthreads();
  } 

  __syncthreads();

  int c_row = warp_m*WMMA_M;
  int c_col = warp_n*WMMA_N;

  if(c_row < M && c_col < N){ 
    wmma::store_matrix_sync(sm_c+c_row+c_col*M, c_frag, M, wmma::mem_col_major);
  }
  __syncthreads();
}
__syncthreads();

o00_re = sm_c[ (((threadIdx.x*3+0)*2+0)*4+0)*param.dc.Ls+coord[4] ];
o00_im = sm_c[ (((threadIdx.x*3+0)*2+1)*4+0)*param.dc.Ls+coord[4] ];
o01_re = sm_c[ (((threadIdx.x*3+1)*2+0)*4+0)*param.dc.Ls+coord[4] ];
o01_im = sm_c[ (((threadIdx.x*3+1)*2+1)*4+0)*param.dc.Ls+coord[4] ];
o02_re = sm_c[ (((threadIdx.x*3+2)*2+0)*4+0)*param.dc.Ls+coord[4] ];
o02_im = sm_c[ (((threadIdx.x*3+2)*2+1)*4+0)*param.dc.Ls+coord[4] ];
o10_re = sm_c[ (((threadIdx.x*3+0)*2+0)*4+1)*param.dc.Ls+coord[4] ];
o10_im = sm_c[ (((threadIdx.x*3+0)*2+1)*4+1)*param.dc.Ls+coord[4] ];
o11_re = sm_c[ (((threadIdx.x*3+1)*2+0)*4+1)*param.dc.Ls+coord[4] ];
o11_im = sm_c[ (((threadIdx.x*3+1)*2+1)*4+1)*param.dc.Ls+coord[4] ];
o12_re = sm_c[ (((threadIdx.x*3+2)*2+0)*4+1)*param.dc.Ls+coord[4] ];
o12_im = sm_c[ (((threadIdx.x*3+2)*2+1)*4+1)*param.dc.Ls+coord[4] ];
o20_re = sm_c[ (((threadIdx.x*3+0)*2+0)*4+2)*param.dc.Ls+coord[4] ];
o20_im = sm_c[ (((threadIdx.x*3+0)*2+1)*4+2)*param.dc.Ls+coord[4] ];
o21_re = sm_c[ (((threadIdx.x*3+1)*2+0)*4+2)*param.dc.Ls+coord[4] ];
o21_im = sm_c[ (((threadIdx.x*3+1)*2+1)*4+2)*param.dc.Ls+coord[4] ];
o22_re = sm_c[ (((threadIdx.x*3+2)*2+0)*4+2)*param.dc.Ls+coord[4] ];
o22_im = sm_c[ (((threadIdx.x*3+2)*2+1)*4+2)*param.dc.Ls+coord[4] ];
o30_re = sm_c[ (((threadIdx.x*3+0)*2+0)*4+3)*param.dc.Ls+coord[4] ];
o30_im = sm_c[ (((threadIdx.x*3+0)*2+1)*4+3)*param.dc.Ls+coord[4] ];
o31_re = sm_c[ (((threadIdx.x*3+1)*2+0)*4+3)*param.dc.Ls+coord[4] ];
o31_im = sm_c[ (((threadIdx.x*3+1)*2+1)*4+3)*param.dc.Ls+coord[4] ];
o32_re = sm_c[ (((threadIdx.x*3+2)*2+0)*4+3)*param.dc.Ls+coord[4] ];
o32_im = sm_c[ (((threadIdx.x*3+2)*2+1)*4+3)*param.dc.Ls+coord[4] ];

} // wmma.h

if(!idle){
// write spinor field back to device memory
WRITE_SPINOR(param.sp_stride);
}

// undefine to prevent warning when precision is changed
#undef m5
#undef mdwf_b5
#undef mdwf_c5
#undef mferm
#undef a
#undef b
#undef POW
#undef SHARED_STRIDE

#undef i00_re
#undef i00_im
#undef i01_re
#undef i01_im
#undef i02_re
#undef i02_im
#undef i10_re
#undef i10_im
#undef i11_re
#undef i11_im
#undef i12_re
#undef i12_im
#undef i20_re
#undef i20_im
#undef i21_re
#undef i21_im
#undef i22_re
#undef i22_im
#undef i30_re
#undef i30_im
#undef i31_re
#undef i31_im
#undef i32_re
#undef i32_im

