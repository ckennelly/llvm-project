; Check that a "cfi" operand bundle naming a vtable keeps the cfi-vcall check
; with a virtual call through sample-profile-driven indirect call promotion in
; the ThinLTO backend: the promoted direct call needs no check (and gets
; inlined), and only the remaining indirect call type-tests the vtable.

; REQUIRES: x86-registered-target

; RUN: rm -rf %t.dir && split-file %s %t.dir
; RUN: opt -thinlto-bc -thinlto-split-lto-unit %t.dir/main.ll -o %t.dir/main.bc
; RUN: llvm-bcanalyzer -dump %t.dir/main.bc | FileCheck %s --check-prefix=SUMMARY
; RUN: llvm-lto2 run -save-temps %t.dir/main.bc -o %t.dir/out \
; RUN:   -lto-sample-profile-file=%t.dir/main.prof \
; RUN:   -r=%t.dir/main.bc,_ZN1A1fEv,plx -r=%t.dir/main.bc,_ZN1A1fEv, \
; RUN:   -r=%t.dir/main.bc,_ZTV1A, -r=%t.dir/main.bc,_ZTV1A,plx \
; RUN:   -r=%t.dir/main.bc,call_it,plx
; RUN: llvm-dis %t.dir/out.1.4.opt.bc -o - | FileCheck %s
; (the vtable and its method each appear in both halves of the split module,
; defined in one and referenced from the other, hence the paired resolutions)

; The vtable's type id is recorded in the function summary from the bundle.
; SUMMARY: <TYPE_TESTS op0=7004155349499253778/>

; CHECK-LABEL: define hidden {{.*}}i32 @call_it(ptr {{.*}}%obj, i32 {{.*}}%x)
; CHECK: %vtable = load ptr, ptr %obj
; CHECK: icmp eq ptr {{.*}}, @_ZN1A1fEv
; CHECK: add i32 %x, 1
; CHECK-NOT: call {{.*}}@_ZN1A1fEv
; CHECK: icmp eq ptr %vtable, @__typeid__ZTS1A_global_addr
; CHECK: call void @llvm.ubsantrap(i8 2)
; CHECK: call i32 %{{.*}}(ptr {{.*}}%obj, i32 %x)
; CHECK-NOT: "cfi"

;--- main.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@_ZTV1A = hidden constant { [3 x ptr] } { [3 x ptr] [ptr null, ptr null, ptr @_ZN1A1fEv] }, !type !0

define hidden i32 @_ZN1A1fEv(ptr %this, i32 %x) #0 !dbg !11 {
  %r = add i32 %x, 1, !dbg !12
  ret i32 %r, !dbg !12
}

define hidden i32 @call_it(ptr %obj, i32 %x) #0 !dbg !7 {
  %vtable = load ptr, ptr %obj, !dbg !10
  %vfn = load ptr, ptr %vtable, !dbg !10
  %r = call i32 %vfn(ptr %obj, i32 %x) [ "cfi"(metadata !"_ZTS1A", ptr %vtable) ], !dbg !10
  ret i32 %r
}

attributes #0 = { "use-sample-profile" }

!llvm.dbg.cu = !{!1}
!llvm.module.flags = !{!3, !4}

!0 = !{i64 16, !"_ZTS1A"}
!1 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !2, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!2 = !DIFile(filename: "main.cc", directory: "/tmp")
!3 = !{i32 2, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!7 = distinct !DISubprogram(name: "call_it", scope: !2, file: !2, line: 3, type: !8, isLocal: false, isDefinition: true, scopeLine: 3, isOptimized: true, unit: !1)
!8 = !DISubroutineType(types: !9)
!9 = !{null}
!10 = !DILocation(line: 4, column: 5, scope: !7)
!11 = distinct !DISubprogram(name: "_ZN1A1fEv", scope: !2, file: !2, line: 7, type: !8, isLocal: false, isDefinition: true, scopeLine: 7, isOptimized: true, unit: !1)
!12 = !DILocation(line: 8, column: 3, scope: !11)

;--- main.prof
call_it:1000:0
 1: 1000 _ZN1A1fEv:1000
_ZN1A1fEv:1000:1000
 1: 1000
