; Whole-program devirtualization with "cfi" operand bundles on the virtual
; calls, through the full ThinLTO flow: A::n has a single implementation and is
; devirtualized to an unchecked direct call; A::f has two (B::f, C::f) and
; stays an indirect call whose vtable is range-checked by LowerTypeTests.

; REQUIRES: x86-registered-target

; RUN: opt -thinlto-bc -thinlto-split-lto-unit -o %t.o %s
; RUN: llvm-lto2 run %t.o -save-temps -pass-remarks=. \
; RUN:   -whole-program-visibility \
; RUN:   -o %t3 \
; RUN:   -r=%t.o,test,px \
; RUN:   -r=%t.o,_ZN1A1nEi,p -r=%t.o,_ZN1A1nEi, \
; RUN:   -r=%t.o,_ZN1B1fEi,p -r=%t.o,_ZN1B1fEi, \
; RUN:   -r=%t.o,_ZN1C1fEi,p -r=%t.o,_ZN1C1fEi, \
; RUN:   -r=%t.o,_ZTV1B, -r=%t.o,_ZTV1B,px \
; RUN:   -r=%t.o,_ZTV1C, -r=%t.o,_ZTV1C,px \
; RUN:   2>&1 | FileCheck %s --check-prefix=REMARK
; RUN: llvm-dis %t3.1.4.opt.bc -o - | FileCheck %s --check-prefix=CHECK-IR
; (functions and vtables appear in both halves of the split module: defined in
; one, referenced from the other, hence the paired resolutions)

; REMARK: single-impl: devirtualized a call to _ZN1A1nEi

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.A = type { ptr }

@_ZTV1B = hidden constant { [4 x ptr] } { [4 x ptr] [ptr null, ptr undef, ptr @_ZN1B1fEi, ptr @_ZN1A1nEi] }, !type !0, !type !1
@_ZTV1C = hidden constant { [4 x ptr] } { [4 x ptr] [ptr null, ptr undef, ptr @_ZN1C1fEi, ptr @_ZN1A1nEi] }, !type !0, !type !2

; CHECK-IR-LABEL: define {{(noundef )?}}i32 @test
define i32 @test(ptr %obj, i32 %a) {
entry:
  %vtable = load ptr, ptr %obj
  %p = call i1 @llvm.type.test(ptr %vtable, metadata !"_ZTS1A")
  call void @llvm.assume(i1 %p)
  %fptrptr = getelementptr ptr, ptr %vtable, i32 1
  %fptr1 = load ptr, ptr %fptrptr, align 8

  ; Devirtualized: direct, no bundle, no check.
  ; CHECK-IR: tail call i32 @_ZN1A1nEi(ptr {{.*}}%obj, i32 %a)
  ; CHECK-IR-NOT: "cfi"
  %call = tail call i32 %fptr1(ptr nonnull %obj, i32 %a) [ "cfi"(metadata !"_ZTS1A", ptr %vtable) ]

  %fptr22 = load ptr, ptr %vtable, align 8

  ; Still virtual: the vtable is range-checked before the call.
  ; CHECK-IR: @__typeid__ZTS1A_
  ; CHECK-IR: call void @llvm.ubsantrap(i8 2)
  ; CHECK-IR: tail call i32 %fptr22(ptr {{.*}}%obj, i32 {{.*}})
  ; CHECK-IR-NOT: "cfi"
  %call3 = tail call i32 %fptr22(ptr nonnull %obj, i32 %call) [ "cfi"(metadata !"_ZTS1A", ptr %vtable) ]
  ret i32 %call3
}
; CHECK-IR-LABEL: ret i32
; CHECK-IR-LABEL: }

define hidden i32 @_ZN1B1fEi(ptr %this, i32 %a) #0 {
   ret i32 0
}

define hidden i32 @_ZN1A1nEi(ptr %this, i32 %a) #0 {
   ret i32 0
}

define hidden i32 @_ZN1C1fEi(ptr %this, i32 %a) #0 {
   ret i32 0
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.assume(i1)

attributes #0 = { noinline optnone }

!0 = !{i64 16, !"_ZTS1A"}
!1 = !{i64 16, !"_ZTS1B"}
!2 = !{i64 16, !"_ZTS1C"}
