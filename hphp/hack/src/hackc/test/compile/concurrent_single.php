<?hh
// RUN: %hackc compile %s | FileCheck %s

// CHECK-LABEL: .function {} {{.*}} single_await() {{.*}} {
// CHECK: FCallFuncD {{.*}} "value"
// CHECK: Await
async function single_await(): Awaitable<void> {
  concurrent {
    await value();
  }
}

// CHECK-LABEL: .function {} {{.*}} single_assignment() {{.*}} {
// CHECK: FCallFuncD {{.*}} "value"
// CHECK: Await
// CHECK: {{(PopL|SetL)}} $result
async function single_assignment(): Awaitable<int> {
  concurrent {
    $result = await value();
  }
  return $result;
}

// CHECK-LABEL: .function {} {{.*}} multiple_awaits() {{.*}} {
// CHECK: FCallFuncD {{.*}} "value"
// CHECK: FCallFuncD {{.*}} "other_value"
// CHECK: AwaitAll
async function multiple_awaits(): Awaitable<int> {
  concurrent {
    $result = await value() + await other_value();
  }
  return $result;
}
