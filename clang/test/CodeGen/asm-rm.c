// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm %s -o - | FileCheck %s
// Register-or-memory constraints ("+r,m", "+rm", "=r,m") on GPR-sized scalars
// are emitted as direct register-first alternatives ("=r|m") so the operand is
// not pinned to a stack slot. See https://github.com/llvm/llvm-project/issues/20571

typedef int v2si __attribute__((vector_size(8)));

int test_rm_int(int x) {
  // CHECK-LABEL: @test_rm_int
  // CHECK: call i32 asm sideeffect "", "=r|m,0,~{memory},~{dirflag},~{fpsr},~{flags}"(i32 %{{.*}})
  __asm__ volatile("" : "+r,m"(x) : : "memory");
  return x;
}

int *test_rm_ptr(int *p) {
  // CHECK-LABEL: @test_rm_ptr
  // CHECK: call ptr asm sideeffect "", "=r|m,0,~{memory},~{dirflag},~{fpsr},~{flags}"(ptr %{{.*}})
  __asm__ volatile("" : "+r,m"(p) : : "memory");
  return p;
}

long test_rm_nocomma(long x) {
  // "+rm" is canonicalized to "r|m" as well.
  // CHECK-LABEL: @test_rm_nocomma
  // CHECK: call i64 asm sideeffect "", "=r|m,0,~{dirflag},~{fpsr},~{flags}"(i64 %{{.*}})
  __asm__ volatile("" : "+rm"(x));
  return x;
}

float test_rm_float(float x) {
  // CHECK-LABEL: @test_rm_float
  // CHECK: call float asm sideeffect "", "=r|m,0,~{dirflag},~{fpsr},~{flags}"(float %{{.*}})
  __asm__ volatile("" : "+r,m"(x));
  return x;
}

int test_rm_out(void) {
  // Pure output.
  // CHECK-LABEL: @test_rm_out
  // CHECK: call i32 asm "movl $$1, $0", "=r|m,~{dirflag},~{fpsr},~{flags}"()
  int x;
  __asm__("movl $1, %0" : "=r,m"(x));
  return x;
}

// ---- Negative cases: these must keep the by-reference (indirect) lowering.

__int128 test_rm_i128(__int128 x) {
  // Wider than a register.
  // CHECK-LABEL: @test_rm_i128
  // CHECK: call void asm sideeffect "", "=*r|m,0,~{dirflag},~{fpsr},~{flags}"(ptr {{.*}}elementtype(i128)
  __asm__ volatile("" : "+r,m"(x));
  return x;
}

v2si test_rm_vec(v2si x) {
  // "r" cannot hold a 64-bit vector on x86-64; must not become a direct output.
  // CHECK-LABEL: @test_rm_vec
  // CHECK: call void asm sideeffect "", "=*r|m,0,~{dirflag},~{fpsr},~{flags}"(ptr {{.*}}elementtype(<2 x i32>)
  __asm__ volatile("" : "+r,m"(x));
  return x;
}

int test_mr_memory_first(int x) {
  // Memory-first spelling keeps the conservative form.
  // CHECK-LABEL: @test_mr_memory_first
  // CHECK: call void asm sideeffect "", "=*m|r,0,~{dirflag},~{fpsr},~{flags}"(ptr {{.*}}elementtype(i32)
  __asm__ volatile("" : "+m,r"(x));
  return x;
}

int test_earlyclobber_rm(int x) {
  // Modifier prefixes keep the conservative form.
  // CHECK-LABEL: @test_earlyclobber_rm
  // CHECK: call void asm sideeffect "", "=*&rm,0,~{dirflag},~{fpsr},~{flags}"(ptr {{.*}}elementtype(i32)
  __asm__ volatile("" : "+&rm"(x));
  return x;
}
