<?hh

interface IRecursiveSuperThisBase {
  abstract const type T as IRecursiveSuperThisBase super this;
  public function get(): this::T;
  public static function gen(): Awaitable<?this::T>;
}

interface IRecursiveSuperThisChild extends IRecursiveSuperThisBase {}

function takes_recursive_super_this_child(
  IRecursiveSuperThisChild $_,
): void {}

function test_recursive_super_this_is_refinement(
  IRecursiveSuperThisBase $base,
): void {
  $value = $base->get();
  if ($value is IRecursiveSuperThisChild) {
    takes_recursive_super_this_child($value);
  }
}

async function test_recursive_super_this_is_refinement_after_await(
  class<IRecursiveSuperThisBase> $base,
): Awaitable<void> {
  $value = await $base::gen();
  if ($value === null || !($value is IRecursiveSuperThisChild)) {
    return;
  }
  takes_recursive_super_this_child($value);
}
