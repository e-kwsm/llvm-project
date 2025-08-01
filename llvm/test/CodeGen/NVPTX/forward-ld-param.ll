; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py UTC_ARGS: --version 5
; RUN: llc < %s | FileCheck %s

target triple = "nvptx64-nvidia-cuda"

define i32 @test_ld_param_const(ptr byval(i32) %a) {
; CHECK-LABEL: test_ld_param_const(
; CHECK:       {
; CHECK-NEXT:    .reg .b32 %r<2>;
; CHECK-NEXT:    .reg .b64 %rd<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    ld.param.b32 %r1, [test_ld_param_const_param_0+4];
; CHECK-NEXT:    st.param.b32 [func_retval0], %r1;
; CHECK-NEXT:    ret;
  %p2 = getelementptr i32, ptr %a, i32 1
  %ld = load i32, ptr %p2
  ret i32 %ld
}

define i32 @test_ld_param_non_const(ptr byval([10 x i32]) %a, i32 %b) {
; CHECK-LABEL: test_ld_param_non_const(
; CHECK:       {
; CHECK-NEXT:    .reg .b32 %r<2>;
; CHECK-NEXT:    .reg .b64 %rd<4>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    mov.b64 %rd1, test_ld_param_non_const_param_0;
; CHECK-NEXT:    ld.param.s32 %rd2, [test_ld_param_non_const_param_1];
; CHECK-NEXT:    add.s64 %rd3, %rd1, %rd2;
; CHECK-NEXT:    ld.local.b32 %r1, [%rd3];
; CHECK-NEXT:    st.param.b32 [func_retval0], %r1;
; CHECK-NEXT:    ret;
  %p2 = getelementptr i8, ptr %a, i32 %b
  %ld = load i32, ptr %p2
  ret i32 %ld
}

declare void @escape(ptr)
declare void @byval_user(ptr byval(i32))

define void @test_ld_param_escaping(ptr byval(i32) %a) {
; CHECK-LABEL: test_ld_param_escaping(
; CHECK:       {
; CHECK-NEXT:    .reg .b64 %rd<3>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    mov.b64 %rd1, test_ld_param_escaping_param_0;
; CHECK-NEXT:    cvta.local.u64 %rd2, %rd1;
; CHECK-NEXT:    { // callseq 0, 0
; CHECK-NEXT:    .param .b64 param0;
; CHECK-NEXT:    st.param.b64 [param0], %rd2;
; CHECK-NEXT:    call.uni escape, (param0);
; CHECK-NEXT:    } // callseq 0
; CHECK-NEXT:    ret;
  call void @escape(ptr %a)
  ret void
}

define void @test_ld_param_byval(ptr byval(i32) %a) {
; CHECK-LABEL: test_ld_param_byval(
; CHECK:       {
; CHECK-NEXT:    .reg .b32 %r<2>;
; CHECK-NEXT:    .reg .b64 %rd<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    { // callseq 1, 0
; CHECK-NEXT:    .param .align 4 .b8 param0[4];
; CHECK-NEXT:    ld.param.b32 %r1, [test_ld_param_byval_param_0];
; CHECK-NEXT:    st.param.b32 [param0], %r1;
; CHECK-NEXT:    call.uni byval_user, (param0);
; CHECK-NEXT:    } // callseq 1
; CHECK-NEXT:    ret;
  call void @byval_user(ptr %a)
  ret void
}

define i32 @test_modify_param(ptr byval([10 x i32]) %a, i32 %b, i32 %c ) {
; CHECK-LABEL: test_modify_param(
; CHECK:       {
; CHECK-NEXT:    .reg .b32 %r<3>;
; CHECK-NEXT:    .reg .b64 %rd<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    mov.b64 %rd1, test_modify_param_param_0;
; CHECK-NEXT:    ld.param.b32 %r1, [test_modify_param_param_1];
; CHECK-NEXT:    ld.param.b32 %r2, [test_modify_param_param_2];
; CHECK-NEXT:    st.local.b32 [%rd1+2], %r1;
; CHECK-NEXT:    st.param.b32 [func_retval0], %r2;
; CHECK-NEXT:    ret;
  %p2 = getelementptr i8, ptr %a, i32 2
  store volatile i32 %b, ptr %p2
  ret i32 %c
}

define i32 @test_multi_block(ptr byval([10 x i32]) %a, i1 %p) {
; CHECK-LABEL: test_multi_block(
; CHECK:       {
; CHECK-NEXT:    .reg .pred %p<3>;
; CHECK-NEXT:    .reg .b16 %rs<3>;
; CHECK-NEXT:    .reg .b32 %r<5>;
; CHECK-NEXT:    .reg .b64 %rd<2>;
; CHECK-EMPTY:
; CHECK-NEXT:  // %bb.0:
; CHECK-NEXT:    ld.param.b8 %rs1, [test_multi_block_param_1];
; CHECK-NEXT:    and.b16 %rs2, %rs1, 1;
; CHECK-NEXT:    setp.ne.b16 %p1, %rs2, 0;
; CHECK-NEXT:    not.pred %p2, %p1;
; CHECK-NEXT:    @%p2 bra $L__BB5_2;
; CHECK-NEXT:  // %bb.1: // %if
; CHECK-NEXT:    ld.param.b32 %r4, [test_multi_block_param_0+4];
; CHECK-NEXT:    bra.uni $L__BB5_3;
; CHECK-NEXT:  $L__BB5_2: // %else
; CHECK-NEXT:    ld.param.b32 %r4, [test_multi_block_param_0+8];
; CHECK-NEXT:  $L__BB5_3: // %end
; CHECK-NEXT:    st.param.b32 [func_retval0], %r4;
; CHECK-NEXT:    ret;
  br i1 %p, label %if, label %else
if:
  %p2 = getelementptr i8, ptr %a, i32 4
  %v2  = load i32, ptr %p2
  br label %end
else:
  %p3 = getelementptr i8, ptr %a, i32 8
  %v3 = load i32, ptr %p3
  br label %end
end:
  %v = phi i32 [ %v2, %if ], [ %v3, %else ]
  ret i32 %v
}
