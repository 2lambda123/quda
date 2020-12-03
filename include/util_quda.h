#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <enum_quda.h>
#include <comm_quda.h>
#include <tune_key.h>
#include <malloc_quda.h>
#include <quda_define.h>
#if defined(QUDA_TARGET_HIP)
#include "hip/hip_runtime_api.h"
#endif


/**
   @brief Query whether autotuning is enabled or not.  Default is enabled but can be overridden by setting QUDA_ENABLE_TUNING=0.
   @return If autotuning is enabled
 */
QudaTune getTuning();

QudaVerbosity getVerbosity();
char *getOutputPrefix();
FILE *getOutputFile();

void setVerbosity(QudaVerbosity verbosity);
void setOutputPrefix(const char *prefix);
void setOutputFile(FILE *outfile);

/**
   @brief Push a new verbosity onto the stack
*/
void pushVerbosity(QudaVerbosity verbosity);

/**
   @brief Pop the verbosity restoring the prior one on the stack
*/
void popVerbosity();

/**
   @brief Push a new output prefix onto the stack
*/
void pushOutputPrefix(const char *prefix);

/**
   @brief Pop the output prefix restoring the prior one on the stack
*/
void popOutputPrefix();

/**
   @brief This function returns true if the calling rank is enabled
   for verbosity (e.g., whether printQuda and warningQuda will print
   out from this rank).
   @return Return whether this rank will print
 */
bool getRankVerbosity();

char *getPrintBuffer();

/**
   @brief Returns a string of the form
   ",omp_threads=$OMP_NUM_THREADS", which can be used for storing the
   number of OMP threads for CPU functions recorded in the tune cache.
   @return Returns the string
*/
char* getOmpThreadStr();

void errorQuda_(const char *func, const char *file, int line, ...);

#if defined(QUDA_TARGET_CUDA)
#define errorQuda(...) do {                                             \
    fprintf(getOutputFile(), "%sERROR: ", getOutputPrefix());           \
    fprintf(getOutputFile(), __VA_ARGS__);                              \
    errorQuda_(__func__, quda::file_name(__FILE__), __LINE__, __VA_ARGS__); \
  } while(0)

#elif defined(QUDA_TARGET_HIP)

#define errorQuda(...) do {                                             \
    printf("%sERROR: ", getOutputPrefix());           \
    printf(__VA_ARGS__);                              \
    errorQuda_(__func__, quda::file_name(__FILE__), __LINE__, __VA_ARGS__); \
  } while(0)

#else 
#error "Unknown QUDA Target"
#endif


#define zeroThread (threadIdx.x + blockDim.x*blockIdx.x==0 &&		\
		    threadIdx.y + blockDim.y*blockIdx.y==0 &&		\
		    threadIdx.z + blockDim.z*blockIdx.z==0)

#define printfZero(...)	do {						\
    if (zeroThread) printf(__VA_ARGS__);				\
  } while (0)

#ifdef MULTI_GPU

#if defined QUDA_TARGET_CUDA
#define printfQuda(...) do {                           \
  sprintf(getPrintBuffer(), __VA_ARGS__);	       \
  if (getRankVerbosity()) {			       \
    fprintf(getOutputFile(), "%s", getOutputPrefix()); \
    fprintf(getOutputFile(), "%s", getPrintBuffer());  \
    fflush(getOutputFile());                           \
  }                                                    \
} while (0)

#define warningQuda(...) do {                                   \
  if (getVerbosity() > QUDA_SILENT) {				\
    sprintf(getPrintBuffer(), __VA_ARGS__);			\
    if (getRankVerbosity()) {						\
      fprintf(getOutputFile(), "%sWARNING: ", getOutputPrefix());	\
      fprintf(getOutputFile(), "%s", getPrintBuffer());			\
      fprintf(getOutputFile(), "\n");					\
      fflush(getOutputFile());						\
    }									\
  }									\
} while (0)
#elif defined(QUDA_TARGET_HIP)

#define printfQuda(...) do {                           \
  sprintf(getPrintBuffer(), __VA_ARGS__);              \
  if (getRankVerbosity()) {                            \
    printf("%s", getOutputPrefix()); \
    printf("%s", getPrintBuffer());  \
  }                                                    \
} while (0)

#define warningQuda(...) do {                                   \
  if (getVerbosity() > QUDA_SILENT) {                           \
    sprintf(getPrintBuffer(), __VA_ARGS__);                     \
    if (getRankVerbosity()) {                                           \
      printf("%sWARNING: ", getOutputPrefix());       \
      printf("%s", getPrintBuffer());                 \
      printf("\n");                                   \
    }                                                                   \
  }                                                                     \
} while (0)

#else
#error "Unknown QUDA Target"
#endif

#else

#if defined(QUDA_TARGET_CUDA)
#define printfQuda(...) do {                         \
  fprintf(getOutputFile(), "%s", getOutputPrefix()); \
  fprintf(getOutputFile(), __VA_ARGS__);             \
  fflush(getOutputFile());                           \
} while (0)

#define warningQuda(...) do {                                 \
  if (getVerbosity() > QUDA_SILENT) {			      \
    fprintf(getOutputFile(), "%sWARNING: ", getOutputPrefix()); \
    fprintf(getOutputFile(), __VA_ARGS__);                      \
    fprintf(getOutputFile(), "\n");                             \
    fflush(getOutputFile());                                    \
  }								\
} while (0)

#elif defined(QUDA_TARGET_HIP)

#define printfQuda(...) do {                         \
  printf("%s", getOutputPrefix()); \
  printf( __VA_ARGS__);             \
} while (0)

#define warningQuda(...) do {                                 \
  if (getVerbosity() > QUDA_SILENT) {                         \
    printf("%sWARNING: ", getOutputPrefix()); \
    printf( __VA_ARGS__);                      \
    printf( "\n");                             \
  }                                                             \
} while (0)


#else
#error "Unknown QUDA_TARGET"
#endif
#endif // MULTI_GPU


#if defined(QUDA_TARGET_CUDA)
#define checkCudaErrorNoSync() do {                    \
  cudaError_t error = cudaGetLastError();              \
  if (error != cudaSuccess)                            \
    errorQuda("(CUDA) %s", cudaGetErrorString(error))  \
    ;\
} while (0)
#elif defined(QUDA_TARGET_HIP)

#define checkCudaErrorNoSync() do {                    \
  hipError_t error = hipGetLastError();              \
  if (error != hipSuccess)                            \
    errorQuda("(HIP) %s", hipGetErrorString(error))  \
    ;\
} while (0)

#endif

#ifdef HOST_DEBUG
#if defined(QUDA_TARGET_CUDA)

#define checkCudaError() do {  \
  cudaDeviceSynchronize();     \
  checkCudaErrorNoSync();      \
} while (0)

#elif defined(QUDA_TARGET_HIP)

#define checkCudaError() do {  \
  hipDeviceSynchronize();     \
  checkCudaErrorNoSync();      \
} while (0)

#endif
#else

#define checkCudaError() checkCudaErrorNoSync()

#endif // HOST_DEBUG
