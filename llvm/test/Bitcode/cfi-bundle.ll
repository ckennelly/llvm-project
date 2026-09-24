; RUN: llvm-as < %s | llvm-dis | FileCheck %s
; RUN: verify-uselistorder < %s

; "cfi" operand bundles round-trip through bitcode.

define void @f(ptr %p) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @f(
; CHECK: call void %p() [ "cfi"(metadata !"_ZTSFvvE") ]
; CHECK: invoke void %p() [ "cfi"(metadata !"_ZTSFvvE") ]
  call void %p() [ "cfi"(metadata !"_ZTSFvvE") ]
  invoke void %p() [ "cfi"(metadata !"_ZTSFvvE") ]
      to label %cont unwind label %lpad

cont:
  ret void

lpad:
  %l = landingpad { ptr, i32 }
      cleanup
  ret void
}

declare i32 @__gxx_personality_v0(...)
