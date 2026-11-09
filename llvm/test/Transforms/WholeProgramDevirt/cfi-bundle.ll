; RUN: opt -S -passes=wholeprogramdevirt -whole-program-visibility %s | FileCheck %s --check-prefixes=CHECK,WPD
; RUN: opt -S -passes='wholeprogramdevirt,instcombine' -whole-program-visibility %s | FileCheck %s --check-prefixes=CHECK,IC

; A "cfi" operand bundle on a virtual call names the vtable it was loaded from.
; Whole-program devirtualization finds the call through the assumed type test
; on that vtable exactly as without the bundle; a call it devirtualizes becomes
; a direct call that still carries the (now vacuous) bundle until InstCombine
; drops it, a call whose result it folds disappears, and a call it funnels is
; replaced by a direct call into the branch funnel without the bundle.

target datalayout = "e-p:64:64"
target triple = "x86_64-unknown-linux-gnu"

;; Single implementation.
@vt1 = constant [1 x ptr] [ptr @vf1], !type !0
@vt2 = constant [1 x ptr] [ptr @vf1], !type !0

define void @vf1(ptr %this) {
  ret void
}

; CHECK-LABEL: define void @single_impl(
; WPD: call void @vf1(ptr %obj) [ "cfi"(metadata !"typeid1", ptr %vtable) ]
; IC:  call void @vf1(ptr {{.*}}%obj)
; IC-NOT: "cfi"
define void @single_impl(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"typeid1")
  call void @llvm.assume(i1 %p)
  %fptr = load ptr, ptr %vtable
  call void %fptr(ptr %obj) [ "cfi"(metadata !"typeid1", ptr %vtable) ]
  ret void
}

;; Uniform return value: the call is folded away, bundle and all.
@vt3 = constant [1 x ptr] [ptr @vf3], !type !1
@vt4 = constant [1 x ptr] [ptr @vf4], !type !1

define i32 @vf3(ptr %this) readnone {
  ret i32 123
}

define i32 @vf4(ptr %this) readnone {
  ret i32 123
}

; CHECK-LABEL: define i32 @uniform_retval(
; CHECK-NOT: call i32 %
; CHECK-NOT: "cfi"
; CHECK: ret i32 123
define i32 @uniform_retval(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"typeid2")
  call void @llvm.assume(i1 %p)
  %fptr = load ptr, ptr %vtable
  %result = call i32 %fptr(ptr %obj) [ "cfi"(metadata !"typeid2", ptr %vtable) ]
  ret i32 %result
}

;; Branch funnel (two implementations, retpoline): the funnel dispatches only
;; among the vtables of the type id, so the check is implied and the bundle is
;; not carried over.
@vt5 = constant [1 x ptr] [ptr @vf5], !type !2
@vt6 = constant [1 x ptr] [ptr @vf6], !type !2

declare i32 @vf5(ptr %this, i32 %arg)
declare i32 @vf6(ptr %this, i32 %arg)

; CHECK-LABEL: define i32 @funnel(
; CHECK: call i32 @__typeid_typeid3_0_branch_funnel(ptr nest %vtable, ptr {{.*}}%obj, i32 1)
; CHECK-NOT: "cfi"
define i32 @funnel(ptr %obj) #0 {
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"typeid3")
  call void @llvm.assume(i1 %p)
  %fptr = load ptr, ptr %vtable
  %result = call i32 %fptr(ptr %obj, i32 1) [ "cfi"(metadata !"typeid3", ptr %vtable) ]
  ret i32 %result
}

;; Not devirtualizable (an implementation is unknown): the call and its bundle
;; are left alone for LowerTypeTests.
@vt7 = constant [1 x ptr] [ptr @vf7], !type !3
@vt8 = constant [1 x ptr] [ptr @vf8], !type !3

declare void @vf7(ptr %this)
declare void @vf8(ptr %this)

; CHECK-LABEL: define void @polymorphic(
; CHECK: call void %fptr(ptr {{.*}}%obj) [ "cfi"(metadata !"typeid4", ptr %vtable) ]
define void @polymorphic(ptr %obj) {
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"typeid4")
  call void @llvm.assume(i1 %p)
  %fptr = load ptr, ptr %vtable
  call void %fptr(ptr %obj) [ "cfi"(metadata !"typeid4", ptr %vtable) ]
  ret void
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.assume(i1)

attributes #0 = { "target-features"="+retpoline" }

!0 = !{i32 0, !"typeid1"}
!1 = !{i32 0, !"typeid2"}
!2 = !{i32 0, !"typeid3"}
!3 = !{i32 0, !"typeid4"}
