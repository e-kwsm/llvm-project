; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S --riscv-v-register-bit-width-lmul=1 | FileCheck %s -check-prefix=LMUL1
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S --riscv-v-register-bit-width-lmul=2 | FileCheck %s -check-prefix=LMUL2
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S --riscv-v-register-bit-width-lmul=4 | FileCheck %s -check-prefix=LMUL4
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S --riscv-v-register-bit-width-lmul=8 | FileCheck %s -check-prefix=LMUL8
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S | FileCheck %s -check-prefix=LMUL2

define void @load_store(ptr %p) {
; LMUL1-LABEL: @load_store(
; LMUL1-NEXT:  entry:
; LMUL1-NEXT:    [[TMP0:%.*]] = call i64 @llvm.vscale.i64()
; LMUL1-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 1024, [[TMP0]]
; LMUL1-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; LMUL1:       vector.ph:
; LMUL1-NEXT:    [[TMP1:%.*]] = call i64 @llvm.vscale.i64()
; LMUL1-NEXT:    [[N_MOD_VF:%.*]] = urem i64 1024, [[TMP1]]
; LMUL1-NEXT:    [[N_VEC:%.*]] = sub i64 1024, [[N_MOD_VF]]
; LMUL1-NEXT:    [[TMP6:%.*]] = call i64 @llvm.vscale.i64()
; LMUL1-NEXT:    br label [[VECTOR_BODY:%.*]]
; LMUL1:       vector.body:
; LMUL1-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; LMUL1-NEXT:    [[TMP3:%.*]] = getelementptr inbounds i64, ptr [[P:%.*]], i64 [[INDEX]]
; LMUL1-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 1 x i64>, ptr [[TMP3]], align 8
; LMUL1-NEXT:    [[TMP5:%.*]] = add <vscale x 1 x i64> [[WIDE_LOAD]], splat (i64 1)
; LMUL1-NEXT:    store <vscale x 1 x i64> [[TMP5]], ptr [[TMP3]], align 8
; LMUL1-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP6]]
; LMUL1-NEXT:    [[TMP7:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; LMUL1-NEXT:    br i1 [[TMP7]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; LMUL1:       middle.block:
; LMUL1-NEXT:    [[CMP_N:%.*]] = icmp eq i64 1024, [[N_VEC]]
; LMUL1-NEXT:    br i1 [[CMP_N]], label [[FOR_END:%.*]], label [[SCALAR_PH]]
; LMUL1:       scalar.ph:
; LMUL1-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; LMUL1-NEXT:    br label [[FOR_BODY:%.*]]
; LMUL1:       for.body:
; LMUL1-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; LMUL1-NEXT:    [[Q:%.*]] = getelementptr inbounds i64, ptr [[P]], i64 [[IV]]
; LMUL1-NEXT:    [[V:%.*]] = load i64, ptr [[Q]], align 8
; LMUL1-NEXT:    [[W:%.*]] = add i64 [[V]], 1
; LMUL1-NEXT:    store i64 [[W]], ptr [[Q]], align 8
; LMUL1-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; LMUL1-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT]], 1024
; LMUL1-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; LMUL1:       for.end:
; LMUL1-NEXT:    ret void
;
; LMUL2-LABEL: @load_store(
; LMUL2-NEXT:  entry:
; LMUL2-NEXT:    [[TMP0:%.*]] = call i64 @llvm.vscale.i64()
; LMUL2-NEXT:    [[TMP1:%.*]] = mul nuw i64 [[TMP0]], 2
; LMUL2-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 1024, [[TMP1]]
; LMUL2-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; LMUL2:       vector.ph:
; LMUL2-NEXT:    [[TMP2:%.*]] = call i64 @llvm.vscale.i64()
; LMUL2-NEXT:    [[TMP3:%.*]] = mul nuw i64 [[TMP2]], 2
; LMUL2-NEXT:    [[N_MOD_VF:%.*]] = urem i64 1024, [[TMP3]]
; LMUL2-NEXT:    [[N_VEC:%.*]] = sub i64 1024, [[N_MOD_VF]]
; LMUL2-NEXT:    [[TMP8:%.*]] = call i64 @llvm.vscale.i64()
; LMUL2-NEXT:    [[TMP9:%.*]] = mul nuw i64 [[TMP8]], 2
; LMUL2-NEXT:    br label [[VECTOR_BODY:%.*]]
; LMUL2:       vector.body:
; LMUL2-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; LMUL2-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i64, ptr [[P:%.*]], i64 [[INDEX]]
; LMUL2-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 2 x i64>, ptr [[TMP5]], align 8
; LMUL2-NEXT:    [[TMP7:%.*]] = add <vscale x 2 x i64> [[WIDE_LOAD]], splat (i64 1)
; LMUL2-NEXT:    store <vscale x 2 x i64> [[TMP7]], ptr [[TMP5]], align 8
; LMUL2-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP9]]
; LMUL2-NEXT:    [[TMP10:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; LMUL2-NEXT:    br i1 [[TMP10]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; LMUL2:       middle.block:
; LMUL2-NEXT:    [[CMP_N:%.*]] = icmp eq i64 1024, [[N_VEC]]
; LMUL2-NEXT:    br i1 [[CMP_N]], label [[FOR_END:%.*]], label [[SCALAR_PH]]
; LMUL2:       scalar.ph:
; LMUL2-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; LMUL2-NEXT:    br label [[FOR_BODY:%.*]]
; LMUL2:       for.body:
; LMUL2-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; LMUL2-NEXT:    [[Q:%.*]] = getelementptr inbounds i64, ptr [[P]], i64 [[IV]]
; LMUL2-NEXT:    [[V:%.*]] = load i64, ptr [[Q]], align 8
; LMUL2-NEXT:    [[W:%.*]] = add i64 [[V]], 1
; LMUL2-NEXT:    store i64 [[W]], ptr [[Q]], align 8
; LMUL2-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; LMUL2-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT]], 1024
; LMUL2-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; LMUL2:       for.end:
; LMUL2-NEXT:    ret void
;
; LMUL4-LABEL: @load_store(
; LMUL4-NEXT:  entry:
; LMUL4-NEXT:    [[TMP0:%.*]] = call i64 @llvm.vscale.i64()
; LMUL4-NEXT:    [[TMP1:%.*]] = mul nuw i64 [[TMP0]], 4
; LMUL4-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 1024, [[TMP1]]
; LMUL4-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; LMUL4:       vector.ph:
; LMUL4-NEXT:    [[TMP2:%.*]] = call i64 @llvm.vscale.i64()
; LMUL4-NEXT:    [[TMP3:%.*]] = mul nuw i64 [[TMP2]], 4
; LMUL4-NEXT:    [[N_MOD_VF:%.*]] = urem i64 1024, [[TMP3]]
; LMUL4-NEXT:    [[N_VEC:%.*]] = sub i64 1024, [[N_MOD_VF]]
; LMUL4-NEXT:    [[TMP8:%.*]] = call i64 @llvm.vscale.i64()
; LMUL4-NEXT:    [[TMP9:%.*]] = mul nuw i64 [[TMP8]], 4
; LMUL4-NEXT:    br label [[VECTOR_BODY:%.*]]
; LMUL4:       vector.body:
; LMUL4-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; LMUL4-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i64, ptr [[P:%.*]], i64 [[INDEX]]
; LMUL4-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 4 x i64>, ptr [[TMP5]], align 8
; LMUL4-NEXT:    [[TMP7:%.*]] = add <vscale x 4 x i64> [[WIDE_LOAD]], splat (i64 1)
; LMUL4-NEXT:    store <vscale x 4 x i64> [[TMP7]], ptr [[TMP5]], align 8
; LMUL4-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP9]]
; LMUL4-NEXT:    [[TMP10:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; LMUL4-NEXT:    br i1 [[TMP10]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; LMUL4:       middle.block:
; LMUL4-NEXT:    [[CMP_N:%.*]] = icmp eq i64 1024, [[N_VEC]]
; LMUL4-NEXT:    br i1 [[CMP_N]], label [[FOR_END:%.*]], label [[SCALAR_PH]]
; LMUL4:       scalar.ph:
; LMUL4-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; LMUL4-NEXT:    br label [[FOR_BODY:%.*]]
; LMUL4:       for.body:
; LMUL4-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; LMUL4-NEXT:    [[Q:%.*]] = getelementptr inbounds i64, ptr [[P]], i64 [[IV]]
; LMUL4-NEXT:    [[V:%.*]] = load i64, ptr [[Q]], align 8
; LMUL4-NEXT:    [[W:%.*]] = add i64 [[V]], 1
; LMUL4-NEXT:    store i64 [[W]], ptr [[Q]], align 8
; LMUL4-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; LMUL4-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT]], 1024
; LMUL4-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; LMUL4:       for.end:
; LMUL4-NEXT:    ret void
;
; LMUL8-LABEL: @load_store(
; LMUL8-NEXT:  entry:
; LMUL8-NEXT:    [[TMP0:%.*]] = call i64 @llvm.vscale.i64()
; LMUL8-NEXT:    [[TMP1:%.*]] = mul nuw i64 [[TMP0]], 8
; LMUL8-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 1024, [[TMP1]]
; LMUL8-NEXT:    br i1 [[MIN_ITERS_CHECK]], label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; LMUL8:       vector.ph:
; LMUL8-NEXT:    [[TMP2:%.*]] = call i64 @llvm.vscale.i64()
; LMUL8-NEXT:    [[TMP3:%.*]] = mul nuw i64 [[TMP2]], 8
; LMUL8-NEXT:    [[N_MOD_VF:%.*]] = urem i64 1024, [[TMP3]]
; LMUL8-NEXT:    [[N_VEC:%.*]] = sub i64 1024, [[N_MOD_VF]]
; LMUL8-NEXT:    [[TMP8:%.*]] = call i64 @llvm.vscale.i64()
; LMUL8-NEXT:    [[TMP9:%.*]] = mul nuw i64 [[TMP8]], 8
; LMUL8-NEXT:    br label [[VECTOR_BODY:%.*]]
; LMUL8:       vector.body:
; LMUL8-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; LMUL8-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i64, ptr [[P:%.*]], i64 [[INDEX]]
; LMUL8-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 8 x i64>, ptr [[TMP5]], align 8
; LMUL8-NEXT:    [[TMP7:%.*]] = add <vscale x 8 x i64> [[WIDE_LOAD]], splat (i64 1)
; LMUL8-NEXT:    store <vscale x 8 x i64> [[TMP7]], ptr [[TMP5]], align 8
; LMUL8-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP9]]
; LMUL8-NEXT:    [[TMP10:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; LMUL8-NEXT:    br i1 [[TMP10]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; LMUL8:       middle.block:
; LMUL8-NEXT:    [[CMP_N:%.*]] = icmp eq i64 1024, [[N_VEC]]
; LMUL8-NEXT:    br i1 [[CMP_N]], label [[FOR_END:%.*]], label [[SCALAR_PH]]
; LMUL8:       scalar.ph:
; LMUL8-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], [[MIDDLE_BLOCK]] ], [ 0, [[ENTRY:%.*]] ]
; LMUL8-NEXT:    br label [[FOR_BODY:%.*]]
; LMUL8:       for.body:
; LMUL8-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; LMUL8-NEXT:    [[Q:%.*]] = getelementptr inbounds i64, ptr [[P]], i64 [[IV]]
; LMUL8-NEXT:    [[V:%.*]] = load i64, ptr [[Q]], align 8
; LMUL8-NEXT:    [[W:%.*]] = add i64 [[V]], 1
; LMUL8-NEXT:    store i64 [[W]], ptr [[Q]], align 8
; LMUL8-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; LMUL8-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT]], 1024
; LMUL8-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; LMUL8:       for.end:
; LMUL8-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %for.body ]
  %q = getelementptr inbounds i64, ptr %p, i64 %iv
  %v = load i64, ptr %q
  %w = add i64 %v, 1
  store i64 %w, ptr %q
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, 1024
  br i1 %exitcond.not, label %for.end, label %for.body

for.end:
  ret void
}
