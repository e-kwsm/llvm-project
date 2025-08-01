; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py UTC_ARGS: --version 5
; RUN: llc --mtriple=loongarch64 --mattr=+lasx --verify-machineinstrs < %s | FileCheck %s

define i32 @xmsk_eq_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_eq_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmsknz.b $xr0, $xr0
; CHECK-NEXT:    xvnor.v $xr0, $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp eq <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i32 @xmsk_sgt_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_sgt_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvrepli.b $xr1, 0
; CHECK-NEXT:    xvslt.b $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp sgt <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i32 @xmsk_sgt_allones_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_sgt_allones_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskgez.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp sgt <32 x i8> %a, splat (i8 -1)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i32 @xmsk_sge_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_sge_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskgez.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp sge <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i32 @xmsk_slt_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_slt_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp slt <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i16 @xmsk_slt_allzeros_i16(<16 x i16 > %a) {
; CHECK-LABEL: xmsk_slt_allzeros_i16:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.h $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 15, 8
; CHECK-NEXT:    ret
entry:
  %1 = icmp slt <16 x i16> %a, splat (i16 0)
  %2 = bitcast <16 x i1> %1 to i16
  ret i16 %2
}

define i8 @xmsk_slt_allzeros_i32(<8 x i32 > %a) {
; CHECK-LABEL: xmsk_slt_allzeros_i32:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
entry:
  %1 = icmp slt <8 x i32> %a, splat (i32 0)
  %2 = bitcast <8 x i1> %1 to i8
  ret i8 %2
}

define i4 @xmsk_slt_allzeros_i64(<4 x i64 > %a) {
; CHECK-LABEL: xmsk_slt_allzeros_i64:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
entry:
  %1 = icmp slt <4 x i64> %a, splat (i64 0)
  %2 = bitcast <4 x i1> %1 to i4
  ret i4 %2
}

define i32 @xmsk_sle_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_sle_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvslei.b $xr0, $xr0, 0
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp sle <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i32 @xmsk_sle_allones_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_sle_allones_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp sle <32 x i8> %a, splat (i8 -1)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i16 @xmsk_sle_allones_i32(<16 x i16 > %a) {
; CHECK-LABEL: xmsk_sle_allones_i32:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.h $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 15, 8
; CHECK-NEXT:    ret
entry:
  %1 = icmp sle <16 x i16> %a, splat (i16 -1)
  %2 = bitcast <16 x i1> %1 to i16
  ret i16 %2
}

define i8 @xmsk_sle_allones_i16(<8 x i32 > %a) {
; CHECK-LABEL: xmsk_sle_allones_i16:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
entry:
  %1 = icmp sle <8 x i32> %a, splat (i32 -1)
  %2 = bitcast <8 x i1> %1 to i8
  ret i8 %2
}

define i4 @xmsk_sle_allones_i64(<4 x i64 > %a) {
; CHECK-LABEL: xmsk_sle_allones_i64:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
entry:
  %1 = icmp sle <4 x i64> %a, splat (i64 -1)
  %2 = bitcast <4 x i1> %1 to i4
  ret i4 %2
}

define i32 @xmsk_ne_allzeros_i8(<32 x i8 > %a) {
; CHECK-LABEL: xmsk_ne_allzeros_i8:
; CHECK:       # %bb.0: # %entry
; CHECK-NEXT:    xvmsknz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
entry:
  %1 = icmp ne <32 x i8> %a, splat (i8 0)
  %2 = bitcast <32 x i1> %1 to i32
  ret i32 %2
}

define i4 @xvmsk_sgt_v4i64(<4 x i64> %a, <4 x i64> %b) {
; CHECK-LABEL: xvmsk_sgt_v4i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.d $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
  %x = icmp sgt <4 x i64> %a, %b
  %res = bitcast <4 x i1> %x to i4
  ret i4 %res
}

define i4 @xvmsk_ogt_v4f64(<4 x double> %a, <4 x double> %b) {
; CHECK-LABEL: xvmsk_ogt_v4f64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.clt.d $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
  %x = fcmp ogt <4 x double> %a, %b
  %res = bitcast <4 x i1> %x to i4
  ret i4 %res
}

define i8 @xvmsk_sgt_v8i32(<8 x i32> %a, <8 x i32> %b) {
; CHECK-LABEL: xvmsk_sgt_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.w $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x = icmp sgt <8 x i32> %a, %b
  %res = bitcast <8 x i1> %x to i8
  ret i8 %res
}

define i8 @xvmsk_ogt_v8f32(<8 x float> %a, <8 x float> %b) {
; CHECK-LABEL: xvmsk_ogt_v8f32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.clt.s $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x = fcmp ogt <8 x float> %a, %b
  %res = bitcast <8 x i1> %x to i8
  ret i8 %res
}

define i16 @xvmsk_sgt_v16i16(<16 x i16> %a, <16 x i16> %b) {
; CHECK-LABEL: xvmsk_sgt_v16i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.h $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.h $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 15, 8
; CHECK-NEXT:    ret
  %x = icmp sgt <16 x i16> %a, %b
  %res = bitcast <16 x i1> %x to i16
  ret i16 %res
}

define i32 @xvmsk_sgt_v32i8(<32 x i8> %a, <32 x i8> %b) {
; CHECK-LABEL: xvmsk_sgt_v32i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.b $xr0, $xr1, $xr0
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
  %x = icmp sgt <32 x i8> %a, %b
  %res = bitcast <32 x i1> %x to i32
  ret i32 %res
}

define i4 @xvmsk_sgt_and_sgt_v4i64(<4 x i64> %a, <4 x i64> %b, <4 x i64> %c, <4 x i64> %d) {
; CHECK-LABEL: xvmsk_sgt_and_sgt_v4i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.d $xr2, $xr3, $xr2
; CHECK-NEXT:    xvslt.d $xr0, $xr1, $xr0
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
  %x0 = icmp sgt <4 x i64> %a, %b
  %x1 = icmp sgt <4 x i64> %c, %d
  %y = and <4 x i1> %x0, %x1
  %res = bitcast <4 x i1> %y to i4
  ret i4 %res
}

define i4 @xvmsk_ogt_and_ogt_v4f64(<4 x double> %a, <4 x double> %b, <4 x double> %c, <4 x double> %d) {
; CHECK-LABEL: xvmsk_ogt_and_ogt_v4f64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.clt.d $xr2, $xr3, $xr2
; CHECK-NEXT:    xvfcmp.clt.d $xr0, $xr1, $xr0
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
  %x0 = fcmp ogt <4 x double> %a, %b
  %x1 = fcmp ogt <4 x double> %c, %d
  %y = and <4 x i1> %x0, %x1
  %res = bitcast <4 x i1> %y to i4
  ret i4 %res
}

define i8 @xvmsk_sgt_and_sgt_v8i32(<8 x i32> %a, <8 x i32> %b, <8 x i32> %c, <8 x i32> %d) {
; CHECK-LABEL: xvmsk_sgt_and_sgt_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.w $xr2, $xr3, $xr2
; CHECK-NEXT:    xvslt.w $xr0, $xr1, $xr0
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = icmp sgt <8 x i32> %a, %b
  %x1 = icmp sgt <8 x i32> %c, %d
  %y = and <8 x i1> %x0, %x1
  %res = bitcast <8 x i1> %y to i8
  ret i8 %res
}

define i8 @xvmsk_sgt_or_sgt_v8i32(<8 x i32> %a, <8 x i32> %b, <8 x i32> %c, <8 x i32> %d) {
; CHECK-LABEL: xvmsk_sgt_or_sgt_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.w $xr2, $xr3, $xr2
; CHECK-NEXT:    xvslt.w $xr0, $xr1, $xr0
; CHECK-NEXT:    xvor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = icmp sgt <8 x i32> %a, %b
  %x1 = icmp sgt <8 x i32> %c, %d
  %y = or <8 x i1> %x0, %x1
  %res = bitcast <8 x i1> %y to i8
  ret i8 %res
}

define i8 @xvmsk_sgt_or_slt_and_eq_v8i32(<8 x i32> %a, <8 x i32> %b, <8 x i32> %c, <8 x i32> %d, <8 x i32> %e, <8 x i32> %f) {
; CHECK-LABEL: xvmsk_sgt_or_slt_and_eq_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.w $xr2, $xr2, $xr3
; CHECK-NEXT:    xvslt.w $xr0, $xr1, $xr0
; CHECK-NEXT:    xvor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvseq.w $xr1, $xr4, $xr5
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr1
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = icmp sgt <8 x i32> %a, %b
  %x1 = icmp slt <8 x i32> %c, %d
  %x2 = icmp eq <8 x i32> %e, %f
  %y = or <8 x i1> %x0, %x1
  %z = and <8 x i1> %y, %x2
  %res = bitcast <8 x i1> %z to i8
  ret i8 %res
}

define i8 @xvmsk_eq_vsel_slt_v8i32(<8 x i32> %a0, <8 x i32> %a1, <8 x i32> %a2) {
; CHECK-LABEL: xvmsk_eq_vsel_slt_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvseq.w $xr0, $xr0, $xr1
; CHECK-NEXT:    xvor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %cmp = icmp eq <8 x i32> %a0, %a1
  %slt = icmp slt <8 x i32> %a2, zeroinitializer
  %sel = select <8 x i1> %cmp, <8 x i1> <i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true, i1 true>, <8 x i1> %slt
  %res = bitcast <8 x i1> %sel to i8
  ret i8 %res
}

define i8 @xvmsk_sel_eq_or_eq_or_slt_v8i32(<8 x i32> %a0, <8 x i32> %a1, <8 x i32> %a2, <8 x i32> %a3, i1 %a4) {
; CHECK-LABEL: xvmsk_sel_eq_or_eq_or_slt_v8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    andi $a0, $a0, 1
; CHECK-NEXT:    xvseq.w $xr2, $xr0, $xr2
; CHECK-NEXT:    addi.d $a1, $zero, -1
; CHECK-NEXT:    maskeqz $a0, $a1, $a0
; CHECK-NEXT:    xvreplgr2vr.w $xr4, $a0
; CHECK-NEXT:    xvand.v $xr2, $xr2, $xr4
; CHECK-NEXT:    xvseq.w $xr0, $xr0, $xr1
; CHECK-NEXT:    xvor.v $xr0, $xr3, $xr0
; CHECK-NEXT:    xvor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %cmp0 = icmp eq <8 x i32> %a0, %a1
  %cmp1 = icmp eq <8 x i32> %a0, %a2
  %cmp2 = icmp slt <8 x i32> %a3, zeroinitializer
  %sel = select i1 %a4, <8 x i1> %cmp1, <8 x i1> zeroinitializer
  %or0 = or <8 x i1> %cmp2, %cmp0
  %or1 = or <8 x i1> %or0, %sel
  %res = bitcast <8 x i1> %or1 to i8
  ret i8 %res
}

define i8 @xvmsk_ogt_and_ogt_v8f32(<8 x float> %a, <8 x float> %b, <8 x float> %c, <8 x float> %d) {
; CHECK-LABEL: xvmsk_ogt_and_ogt_v8f32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.clt.s $xr2, $xr3, $xr2
; CHECK-NEXT:    xvfcmp.clt.s $xr0, $xr1, $xr0
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = fcmp ogt <8 x float> %a, %b
  %x1 = fcmp ogt <8 x float> %c, %d
  %y = and <8 x i1> %x0, %x1
  %res = bitcast <8 x i1> %y to i8
  ret i8 %res
}

define i8 @xvmsk_sgt_xor_sgt_v8f32(<8 x float> %a, <8 x float> %b, <8 x float> %c, <8 x float> %d) {
; CHECK-LABEL: xvmsk_sgt_xor_sgt_v8f32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.clt.s $xr2, $xr3, $xr2
; CHECK-NEXT:    xvfcmp.clt.s $xr0, $xr1, $xr0
; CHECK-NEXT:    xvxor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = fcmp ogt <8 x float> %a, %b
  %x1 = fcmp ogt <8 x float> %c, %d
  %y = xor <8 x i1> %x0, %x1
  %res = bitcast <8 x i1> %y to i8
  ret i8 %res
}

define i8 @xvmsk_ugt_xor_ueq_and_ogt_v8f32(<8 x float> %a, <8 x float> %b, <8 x float> %c, <8 x float> %d, <8 x float> %e, <8 x float> %f) {
; CHECK-LABEL: xvmsk_ugt_xor_ueq_and_ogt_v8f32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvfcmp.cueq.s $xr2, $xr2, $xr3
; CHECK-NEXT:    xvfcmp.cult.s $xr0, $xr1, $xr0
; CHECK-NEXT:    xvxor.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvfcmp.clt.s $xr1, $xr5, $xr4
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr1
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %x0 = fcmp ugt <8 x float> %a, %b
  %x1 = fcmp ueq <8 x float> %c, %d
  %x2 = fcmp ogt <8 x float> %e, %f
  %y = xor <8 x i1> %x0, %x1
  %z = and <8 x i1> %y, %x2
  %res = bitcast <8 x i1> %z to i8
  ret i8 %res
}

define i16 @xvmsk_sgt_and_sgt_v16i16(<16 x i16> %a, <16 x i16> %b, <16 x i16> %c, <16 x i16> %d) {
; CHECK-LABEL: xvmsk_sgt_and_sgt_v16i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.h $xr2, $xr3, $xr2
; CHECK-NEXT:    xvslt.h $xr0, $xr1, $xr0
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr2
; CHECK-NEXT:    xvmskltz.h $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 15, 8
; CHECK-NEXT:    ret
  %x0 = icmp sgt <16 x i16> %a, %b
  %x1 = icmp sgt <16 x i16> %c, %d
  %y = and <16 x i1> %x0, %x1
  %res = bitcast <16 x i1> %y to i16
  ret i16 %res
}

define i32 @xvmsk_sgt_and_sgt_v32i8(<32 x i8> %a, <32 x i8> %b, <32 x i8> %c, <32 x i8> %d) {
; CHECK-LABEL: xvmsk_sgt_and_sgt_v32i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslt.b $xr0, $xr1, $xr0
; CHECK-NEXT:    xvslt.b $xr1, $xr3, $xr2
; CHECK-NEXT:    xvand.v $xr0, $xr0, $xr1
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
  %x0 = icmp sgt <32 x i8> %a, %b
  %x1 = icmp sgt <32 x i8> %c, %d
  %y = and <32 x i1> %x0, %x1
  %res = bitcast <32 x i1> %y to i32
  ret i32 %res
}

define i8 @xvmsk_eq_v2i64_concat_poison(<2 x i64> %vec) {
; CHECK-LABEL: xvmsk_eq_v2i64_concat_poison:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vseqi.d $vr0, $vr0, 0
; CHECK-NEXT:    vpickve2gr.d $a0, $vr0, 0
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 0
; CHECK-NEXT:    vpickve2gr.d $a0, $vr0, 1
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 1
; CHECK-NEXT:    vslli.h $vr0, $vr1, 15
; CHECK-NEXT:    vmskltz.h $vr0, $vr0
; CHECK-NEXT:    vpickve2gr.hu $a0, $vr0, 0
; CHECK-NEXT:    ret
  %tobool = icmp eq <2 x i64> %vec, zeroinitializer
  %insertvec = shufflevector <2 x i1> %tobool, <2 x i1> poison, <8 x i32> <i32 0, i32 1, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison>
  %res = bitcast <8 x i1> %insertvec to i8
  ret i8 %res
}

define i8 @xvmsk_ne_v4i32_concat_poison(<4 x i32> %vec) {
; CHECK-LABEL: xvmsk_ne_v4i32_concat_poison:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vseqi.w $vr0, $vr0, 0
; CHECK-NEXT:    vrepli.b $vr1, -1
; CHECK-NEXT:    vxor.v $vr0, $vr0, $vr1
; CHECK-NEXT:    vpickve2gr.w $a0, $vr0, 0
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 0
; CHECK-NEXT:    vpickve2gr.w $a0, $vr0, 1
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 1
; CHECK-NEXT:    vpickve2gr.w $a0, $vr0, 2
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 2
; CHECK-NEXT:    vpickve2gr.w $a0, $vr0, 3
; CHECK-NEXT:    vinsgr2vr.h $vr1, $a0, 3
; CHECK-NEXT:    vslli.h $vr0, $vr1, 15
; CHECK-NEXT:    vmskltz.h $vr0, $vr0
; CHECK-NEXT:    vpickve2gr.hu $a0, $vr0, 0
; CHECK-NEXT:    ret
  %tobool = icmp ne <4 x i32> %vec, zeroinitializer
  %insertvec = shufflevector <4 x i1> %tobool, <4 x i1> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 poison, i32 poison, i32 poison, i32 poison>
  %res = bitcast <8 x i1> %insertvec to i8
  ret i8 %res
}

define i8 @xvmsk_ogt_v4f64_concat_poison(<4 x double> %vec) {
; CHECK-LABEL: xvmsk_ogt_v4f64_concat_poison:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvrepli.b $xr1, 0
; CHECK-NEXT:    xvfcmp.clt.d $xr0, $xr1, $xr0
; CHECK-NEXT:    xvpickve2gr.d $a0, $xr0, 3
; CHECK-NEXT:    xvpickve2gr.d $a1, $xr0, 2
; CHECK-NEXT:    xvpickve2gr.d $a2, $xr0, 1
; CHECK-NEXT:    xvpickve2gr.d $a3, $xr0, 0
; CHECK-NEXT:    vinsgr2vr.h $vr0, $a3, 0
; CHECK-NEXT:    vinsgr2vr.h $vr0, $a2, 1
; CHECK-NEXT:    vinsgr2vr.h $vr0, $a1, 2
; CHECK-NEXT:    vinsgr2vr.h $vr0, $a0, 3
; CHECK-NEXT:    vslli.h $vr0, $vr0, 15
; CHECK-NEXT:    vmskltz.h $vr0, $vr0
; CHECK-NEXT:    vpickve2gr.hu $a0, $vr0, 0
; CHECK-NEXT:    ret
  %tobool = fcmp ogt <4 x double> %vec, zeroinitializer
  %insertvec = shufflevector <4 x i1> %tobool, <4 x i1> poison, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 poison, i32 poison, i32 poison, i32 poison>
  %res = bitcast <8 x i1> %insertvec to i8
  ret i8 %res
}

define i32 @xvmsk_trunc_i8(<32 x i8> %a) {
; CHECK-LABEL: xvmsk_trunc_i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslli.b $xr0, $xr0, 7
; CHECK-NEXT:    xvmskltz.b $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 31, 16
; CHECK-NEXT:    ret
  %y = trunc <32 x i8> %a to <32 x i1>
  %res = bitcast <32 x i1> %y to i32
  ret i32 %res
}

define i16 @xvmsk_trunc_i16(<16 x i16> %a) {
; CHECK-LABEL: xvmsk_trunc_i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslli.h $xr0, $xr0, 15
; CHECK-NEXT:    xvmskltz.h $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 15, 8
; CHECK-NEXT:    ret
  %y = trunc <16 x i16> %a to <16 x i1>
  %res = bitcast <16 x i1> %y to i16
  ret i16 %res
}

define i8 @xvmsk_trunc_i32(<8 x i32> %a) {
; CHECK-LABEL: xvmsk_trunc_i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslli.w $xr0, $xr0, 31
; CHECK-NEXT:    xvmskltz.w $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 7, 4
; CHECK-NEXT:    ret
  %y = trunc <8 x i32> %a to <8 x i1>
  %res = bitcast <8 x i1> %y to i8
  ret i8 %res
}

define i4 @xvmsk_trunc_i64(<4 x i64> %a) {
; CHECK-LABEL: xvmsk_trunc_i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    xvslli.d $xr0, $xr0, 63
; CHECK-NEXT:    xvmskltz.d $xr0, $xr0
; CHECK-NEXT:    xvpickve2gr.wu $a0, $xr0, 0
; CHECK-NEXT:    xvpickve2gr.wu $a1, $xr0, 4
; CHECK-NEXT:    bstrins.d $a0, $a1, 3, 2
; CHECK-NEXT:    ret
  %y = trunc <4 x i64> %a to <4 x i1>
  %res = bitcast <4 x i1> %y to i4
  ret i4 %res
}
