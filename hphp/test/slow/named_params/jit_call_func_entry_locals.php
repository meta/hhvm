<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

final class CallFuncEntryBox {
  public function __construct(public int $value) {}
}

<<__NEVER_INLINE>>
function call_func_entry_target<reify T>(
  (function()[_]: int) $context,
  CallFuncEntryBox $head,
  named CallFuncEntryBox $a,
  named ?CallFuncEntryBox $b = null,
  named CallFuncEntryBox $c,
  CallFuncEntryBox ...$rest
)[ctx $context]: int {
  $_type = HH\ReifiedGenerics\get_type_structure<T>();
  $sum = $context() + $head->value + $a->value + ($b?->value ?? 0) + $c->value;
  foreach ($rest as $box) {
    $sum += $box->value;
  }
  return $sum + count($_type);
}

<<__NEVER_INLINE>>
function call_func_entry_inout(inout int $output, int $delta): int {
  $output += $delta;
  return $output;
}

<<__EntryPoint>>
function main(): void {
  $context = ()[] ==> 13;
  $output = 0;
  $expected_output = 0;
  $type_size = count(
    HH\ReifiedGenerics\get_type_structure<CallFuncEntryBox>(),
  );
  $failures = 0;

  for ($i = 0; $i < 4000; ++$i) {
    $value = __hhvm_intrinsics\launder_value($i % 17);
    $expected_result = $value + 39 + $type_size;
    $expected_output += $expected_result;

    $result = call_func_entry_target<CallFuncEntryBox>(
      $context,
      new CallFuncEntryBox($value),
      a = new CallFuncEntryBox(3),
      c = new CallFuncEntryBox(5),
      new CallFuncEntryBox(7),
      new CallFuncEntryBox(11),
    );
    $inout_result = call_func_entry_inout(inout $output, $result);

    if ($result !== $expected_result ||
        $output !== $expected_output ||
        $inout_result !== $expected_output) {
      ++$failures;
    }
  }

  echo $failures === 0 ? "ok\n" : "failures: $failures\n";
}
