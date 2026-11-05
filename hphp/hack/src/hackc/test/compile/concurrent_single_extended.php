<?hh
// RUN: %hackc compile -vHack.Lang.AllowUnstableFeatures=1 %s | FileCheck %s
<<file:__EnableUnstableFeatures('pipe_await', 'allow_extended_await_syntax')>>

// CHECK-LABEL: .function {} {{.*}} nested_await() {{.*}} {
// CHECK: FCallFuncD {{.*}} "value"
// CHECK: Await
// CHECK: FCallFuncD {{.*}} "identity"
// CHECK: Await
// CHECK: {{(PopL|SetL)}} $result
async function nested_await(): Awaitable<int> {
  concurrent {
    $result = await identity(await value());
  }
  return $result;
}

// CHECK-LABEL: .function {} {{.*}} piped_await() {{.*}} {
// CHECK: FCallFuncD {{.*}} "value"
// CHECK: Await
// CHECK: FCallFuncD {{.*}} "identity"
// CHECK: Await
// CHECK: {{(PopL|SetL)}} $result
async function piped_await(): Awaitable<int> {
  concurrent {
    $result = await value() |> await identity($$);
  }
  return $result;
}
