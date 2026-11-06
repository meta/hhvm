<?hh

module A;

<<__EntryPoint>>
function main(): void {
  include 'fold-1.inc';
  // HHBBC knows the class, but folding the call would skip its module check.
  $class = 'C';
  try {
    echo $class::answer()."\n";
  } catch (Exception $e) {
    echo $e->getMessage()."\n";
  }
}
