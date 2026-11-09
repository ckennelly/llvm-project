// RUN: %clang_cc1 -triple x86_64-unknown-linux -O1 -disable-llvm-passes -fvisibility=hidden -fsanitize=cfi-vcall -fsanitize-trap=cfi-vcall -fsanitize-cfi-icall-experimental-operand-bundle -emit-llvm -o - %s | FileCheck %s --check-prefix=BUNDLE
// RUN: %clang_cc1 -triple x86_64-unknown-linux -O1 -disable-llvm-passes -fvisibility=hidden -fsanitize=cfi-vcall -fsanitize-trap=cfi-vcall -emit-llvm -o - %s | FileCheck %s --check-prefix=EXPLICIT
// With whole-program vtables the devirtualization hint is the plain type test
// and assume, as without CFI, and the check is still the bundle.
// RUN: %clang_cc1 -triple x86_64-unknown-linux -O1 -disable-llvm-passes -fvisibility=hidden -fsanitize=cfi-vcall -fsanitize-trap=cfi-vcall -fwhole-program-vtables -fsanitize-cfi-icall-experimental-operand-bundle -emit-llvm -o - %s | FileCheck %s --check-prefix=WPD
// Virtual function elimination is defined in terms of the type checked load,
// which therefore stays.
// RUN: %clang_cc1 -triple x86_64-unknown-linux -O1 -disable-llvm-passes -fvisibility=hidden -fsanitize=cfi-vcall -fsanitize-trap=cfi-vcall -fwhole-program-vtables -fvirtual-function-elimination -fsanitize-cfi-icall-experimental-operand-bundle -emit-llvm -o - %s | FileCheck %s --check-prefix=VFE
// A class with public LTO visibility is not checked either way.
// RUN: %clang_cc1 -triple x86_64-unknown-linux -O1 -disable-llvm-passes -fsanitize=cfi-vcall -fsanitize-trap=cfi-vcall -fsanitize-cfi-icall-experimental-operand-bundle -emit-llvm -o - %s | FileCheck %s --check-prefix=PUBLIC

struct A {
  virtual void f();
  void g();
};

// BUNDLE-LABEL: define hidden void @_Z5vcallP1A(
// BUNDLE: %vtable = load ptr, ptr %{{.*}}, align 8
// BUNDLE-NOT: @llvm.type.test
// BUNDLE: %[[VFN:.*]] = load ptr, ptr %vfn, align 8
// BUNDLE-NEXT: call void %[[VFN]](ptr {{.*}}) [ "cfi"(metadata !"_ZTS1A", ptr %vtable) ]
// BUNDLE-NOT: @llvm.ubsantrap

// EXPLICIT-LABEL: define hidden void @_Z5vcallP1A(
// EXPLICIT: %vtable = load ptr, ptr %{{.*}}, align 8
// EXPLICIT: %[[TEST:.*]] = call i1 @llvm.type.test(ptr %vtable, metadata !"_ZTS1A")
// EXPLICIT: br i1 %[[TEST]]
// EXPLICIT: call void @llvm.ubsantrap(i8 2)
// EXPLICIT: call void %{{.*}}(ptr {{.*}})
// EXPLICIT-NOT: "cfi"

// WPD-LABEL: define hidden void @_Z5vcallP1A(
// WPD: %vtable = load ptr, ptr %{{.*}}, align 8
// WPD-NEXT: %[[TEST:.*]] = call i1 @llvm.type.test(ptr %vtable, metadata !"_ZTS1A")
// WPD-NEXT: call void @llvm.assume(i1 %[[TEST]])
// WPD-NOT: @llvm.type.checked.load
// WPD: %[[VFN:.*]] = load ptr, ptr %vfn, align 8
// WPD-NEXT: call void %[[VFN]](ptr {{.*}}) [ "cfi"(metadata !"_ZTS1A", ptr %vtable) ]

// VFE-LABEL: define hidden void @_Z5vcallP1A(
// VFE: @llvm.type.checked.load(ptr %{{.*}}, i32 0, metadata !"_ZTS1A")
// VFE-NOT: "cfi"

// PUBLIC-LABEL: define{{.*}} void @_Z5vcallP1A(
// PUBLIC-NOT: @llvm.type.test
// PUBLIC-NOT: "cfi"
void vcall(A *a) { a->f(); }

// A non-virtual call through the object is not a cfi-vcall check and stays
// a plain direct call.
// BUNDLE-LABEL: define hidden void @_Z6nvcallP1A(
// BUNDLE: call void @_ZN1A1gEv(ptr {{.*}})
// BUNDLE-NOT: "cfi"
void nvcall(A *a) { a->g(); }
