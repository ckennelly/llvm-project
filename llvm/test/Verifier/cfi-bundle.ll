; RUN: rm -rf %t && split-file %s %t
; RUN: not opt -passes=verify -disable-output %t/two.ll 2>&1 | FileCheck %s --check-prefix=TWO
; RUN: not opt -passes=verify -disable-output %t/count.ll 2>&1 | FileCheck %s --check-prefix=COUNT
; RUN: not opt -passes=verify -disable-output %t/operand.ll 2>&1 | FileCheck %s --check-prefix=OPERAND
; RUN: opt -passes=verify -disable-output %t/ok.ll

;--- two.ll
; TWO: Multiple cfi operand bundles
define void @f(ptr %p) {
  call void %p() [ "cfi"(metadata !"t"), "cfi"(metadata !"t") ]
  ret void
}

;--- count.ll
; COUNT: Expected exactly one cfi bundle operand
define void @f(ptr %p) {
  call void %p() [ "cfi"(metadata !"t", metadata !"u") ]
  ret void
}

;--- operand.ll
; OPERAND: Cfi bundle operand must be a metadata type identifier
define void @f(ptr %p) {
  call void %p() [ "cfi"(i32 0) ]
  ret void
}

;--- ok.ll
; An internal type identifier is a distinct node rather than a string, as for
; llvm.type.test.
define void @f(ptr %p) {
  call void %p() [ "cfi"(metadata !"t") ]
  call void %p() [ "cfi"(metadata !0) ]
  ret void
}

!0 = distinct !{}
