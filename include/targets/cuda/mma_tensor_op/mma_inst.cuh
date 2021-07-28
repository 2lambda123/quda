#pragma once

#include <cuda_bf16.h>

using bfloat162 = __nv_bfloat162;
using bfloat16 = __nv_bfloat16;

struct tfloat32 { };

template <int inst_m, int inst_n, int inst_k, class AB, class CD>
struct MmaInst { };

template <>
struct MmaInst <16, 8, 16, bfloat16, float> {
  __device__ void operator()(float c[4], const unsigned a[4], const unsigned b[2]) {
    asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
        : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
  }
};

template <>
struct MmaInst <16, 8, 8, bfloat16, float> {
  __device__ void operator()(float c[4], const unsigned a[2], const unsigned b[1]) {
    asm volatile("mma.sync.aligned.m16n8k8.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5}, {%6}, {%0,%1,%2,%3};"
        : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(b[0]));
  }
};

template <>
struct MmaInst <16, 8, 8, tfloat32, float> {
  __device__ void operator()(float c[4], const unsigned a[4], const unsigned b[2]) {
    asm volatile("mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
        : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
  }
};

template <>
struct MmaInst <16, 8, 4, tfloat32, float> {
  __device__ void operator()(float c[4], const unsigned a[2], const unsigned b[1]) {
    asm volatile("mma.sync.aligned.m16n8k4.row.col.f32.tf32.tf32.f32 {%0,%1,%2,%3}, {%4,%5}, {%6}, {%0,%1,%2,%3};"
        : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(b[0]));
  }
};

