<?hh

<<__MockClass>>
class StandaloneMock {}

<<__EntryPoint>>
function main(): void {
  var_dump(class_exists(__hhvm_intrinsics\launder_value('StandaloneMock')));
}
