// RUN: mlir-opt %s -pass-pipeline="builtin.module(inline)" -dump-pass-pipeline 2>&1 | FileCheck %s
// CHECK:      builtin.module(
// CHECK-NEXT:   inline{default-pipeline=canonicalize inlining-threshold=4294967295 max-iterations=4 }
// CHECK-NEXT: )
