// REQUIRES: system-windows

// RUN: %clang -### --target=x86_64-pc-windows-msvc \
// RUN:   -x hip \
// RUN:   --offload-arch=gfx908:xnack+ \
// RUN:   -nogpuinc -nogpulib -save-temps \
// RUN:   %s 2>&1 | FileCheck %s

// CHECK: "-o" "hip-windows-filename-hip-amdgcn-amd-amdhsa-gfx908@xnack+.hipi"
// CHECK-NOT: "-o" "hip-windows-filename-hip-amdgcn-amd-amdhsa-gfx908:xnack+.hipi"
