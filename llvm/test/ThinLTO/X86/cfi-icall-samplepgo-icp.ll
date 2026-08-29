; Check that in the ThinLTO backend, indirect call promotion driven by a sample
; profile can promote to a function that is a CFI jump table member. This
; requires the sample profile loader and ICP to run before LowerTypeTests, which
; renames the body of such a function to <name>.cfi.

; REQUIRES: x86-registered-target

; RUN: rm -rf %t.dir && split-file %s %t.dir
; RUN: opt -thinlto-bc -thinlto-split-lto-unit %t.dir/main.ll -o %t.dir/main.bc
; RUN: llvm-lto2 run -save-temps %t.dir/main.bc -o %t.dir/out \
; RUN:   -lto-sample-profile-file=%t.dir/main.prof \
; RUN:   -r=%t.dir/main.bc,add1,plx \
; RUN:   -r=%t.dir/main.bc,table,plx \
; RUN:   -r=%t.dir/main.bc,call_it,plx
; RUN: llvm-dis %t.dir/out.1.4.opt.bc -o - | FileCheck %s

; The sample profile loader saw @add1 under its original name and annotated it
; with an entry count before it was renamed to @add1.cfi. (The count is the
; head sample count plus one, minus the count of the promoted call site that
; was inlined below.)
; CHECK: define hidden {{.*}}i32 @add1.cfi(i32 {{.*}}%x){{.*}} !prof ![[ENTRY:[0-9]+]]

; The indirect call was promoted. The comparison is against the jump table
; entry (@add1) and the direct call targets the body (@add1.cfi), which was
; then inlined.
; CHECK-LABEL: define hidden {{.*}}i32 @call_it(ptr {{.*}}%f, i32 {{.*}}%x)
; CHECK: icmp eq ptr {{.*}}, @add1
; CHECK: add i32 %x, 1
; CHECK-NOT: call {{.*}}@add1
; CHECK: call i32 %f(i32 %x)

; CHECK: ![[ENTRY]] = !{!"function_entry_count", i64 {{[0-9]+}}}

;--- main.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; @add1 must be address taken, or it would be dead-stripped and never get a
; jump table entry.
@table = hidden constant ptr @add1

define hidden i32 @add1(i32 %x) #0 !type !0 !dbg !11 {
  %r = add i32 %x, 1, !dbg !12
  ret i32 %r, !dbg !12
}

define hidden i32 @call_it(ptr %f, i32 %x) #0 !dbg !7 {
  %t = call i1 @llvm.type.test(ptr %f, metadata !"_ZTSFiiE")
  br i1 %t, label %cont, label %trap

trap:
  call void @llvm.ubsantrap(i8 2)
  unreachable

cont:
  %r = call i32 %f(i32 %x), !dbg !10
  ret i32 %r
}

declare i1 @llvm.type.test(ptr, metadata)
declare void @llvm.ubsantrap(i8)

attributes #0 = { "use-sample-profile" }

!llvm.dbg.cu = !{!1}
!llvm.module.flags = !{!3, !4}

!0 = !{i64 0, !"_ZTSFiiE"}
!1 = distinct !DICompileUnit(language: DW_LANG_C99, file: !2, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!2 = !DIFile(filename: "main.c", directory: "/tmp")
!3 = !{i32 2, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!7 = distinct !DISubprogram(name: "call_it", scope: !2, file: !2, line: 3, type: !8, isLocal: false, isDefinition: true, scopeLine: 3, isOptimized: true, unit: !1)
!8 = !DISubroutineType(types: !9)
!9 = !{null}
!10 = !DILocation(line: 4, column: 5, scope: !7)
!11 = distinct !DISubprogram(name: "add1", scope: !2, file: !2, line: 7, type: !8, isLocal: false, isDefinition: true, scopeLine: 7, isOptimized: true, unit: !1)
!12 = !DILocation(line: 8, column: 3, scope: !11)

;--- main.prof
call_it:1000:0
 1: 1000 add1:1000
add1:1000:1000
 1: 1000
