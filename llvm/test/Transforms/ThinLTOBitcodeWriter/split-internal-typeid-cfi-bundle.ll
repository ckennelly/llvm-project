; RUN: opt -thinlto-bc -thinlto-split-lto-unit -o %t %s
; RUN: llvm-modextract -b -n 0 -o %t0 %t
; RUN: llvm-modextract -b -n 1 -o %t1 %t
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=M0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=M1 %s

; An internal type id in a "cfi" operand bundle is promoted to the same
; module-unique string as the function's !type metadata, like the type id of an
; llvm.type.test call.

; M0: define hidden void @f() !type [[TYPE:![0-9]+]]
define hidden void @f() !type !0 {
  ret void
}

@fptr = global ptr @f

; M0: define void @call(
define void @call(ptr %p, ptr %vtable) {
  ; M0: call void %p() [ "cfi"(metadata !"[[ID:1\.[0-9a-f]+]]") ]
  call void %p() [ "cfi"(metadata !1) ]
  ; The checked pointer of a virtual call's bundle is kept.
  ; M0: call void %p() [ "cfi"(metadata !"[[ID]]", ptr %vtable) ]
  call void %p() [ "cfi"(metadata !1, ptr %vtable) ]
  ret void
}

; M0: [[TYPE]] = !{i64 0, !"[[ID]]"}
; M1: !cfi.functions = !{[[F:![0-9]+]]}
; M1: [[F]] = !{!"f", i8 0, i64 {{-?[0-9]+}}, [[FT:![0-9]+]]}
; M1: [[FT]] = !{i64 0, !"1.{{[0-9a-f]+}}"}

!0 = !{i64 0, !1}
!1 = distinct !{}
