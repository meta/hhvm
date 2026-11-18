<?hh

final class FinalBase {
  public function getValue(): int {
    return 1;
  }
}

<<__MockClass>>
class MockOverride extends FinalBase {
  public function getValue(): int {
    return 2;
  }
}

<<__EntryPoint>>
function main(): void {
  var_dump(class_exists(__hhvm_intrinsics\launder_value('MockOverride')));
}
