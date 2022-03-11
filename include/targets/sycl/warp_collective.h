namespace quda {

  template <int warp_split, typename T> inline T warp_combine(T &x)
  {
    //__syncthreads();
    //auto sg = sycl::ONEAPI::this_sub_group();
    auto sg = sycl::ext::oneapi::experimental::this_sub_group();
    constexpr int warp_size = device::warp_size();
    //const int warp_size = min(device::warp_size(),
    //			      (int)getGroup().get_local_range().size());
    const int sg_size = sg.get_local_range().size();
    if (warp_split > 1) {
#pragma unroll
      for (int i = 0; i < x.size(); i++) {
        // reduce down to the first group of column-split threads
#pragma unroll
        for (int offset = sg_size / 2; offset >= warp_size / warp_split; offset /= 2) {
          x[i].real(x[i].real() + sg.shuffle_down(x[i].real(), offset));
          x[i].imag(x[i].imag() + sg.shuffle_down(x[i].imag(), offset));
        }
      }
    }
    return x;
  }

}