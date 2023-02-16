#pragma once

namespace quda {

  namespace device {

    constexpr int dynamic_shared_memory_supremum() {
#if (__COMPUTE_CAPABILITY__ < 700)
      return 48 * 1024;
#elif (__COMPUTE_CAPABILITY__ < 750) // 700, 720
      return 96 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 750)
      return 64 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 800)
      return 164 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 860)
      return 100 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 870)
      return 164 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 890)
      return 100 * 1024;
#elif (__COMPUTE_CAPABILITY__ == 900)
      return 228 * 1024;
#else
      return 0;
#endif
    }

  }

}