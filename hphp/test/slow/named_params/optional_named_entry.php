<?hh
<<file: __EnableUnstableFeatures('named_parameters')>>

function string_default(string $s = '', named bool $a = false): string {
  return $a ? 'passed' : 'default';
}

function string_default_two(
  string $s = '',
  named bool $a = false,
  named bool $b = false,
): string {
  return $a ? 'passed' : 'default';
}

function partial_positional(
  string $s = '',
  string $t = '',
  named bool $a = false,
  named bool $b = false,
): string {
  return $a ? 'passed' : 'default';
}

function vec_default(
  vec<int> $v = vec[],
  named bool $a = false,
  named bool $b = false,
): string {
  return $a ? 'passed' : 'default';
}

class DefaultConstructor {
  public string $result;

  public function __construct(
    string $s = '',
    named bool $a = false,
    named bool $b = false,
  ) {
    $this->result = $a ? 'passed' : 'default';
  }
}

<<__EntryPoint>>
function main(): void {
  // Profile both omitted and supplied named arguments at the same DV entry.
  try {
    echo 'one: '.string_default().' '.string_default(a = true)."\n";
  } catch (Exception $e) {
    echo 'one: '.$e->getMessage()."\n";
  }
  try {
    echo 'two: '.string_default_two().' '.string_default_two(a = true)."\n";
  } catch (Exception $e) {
    echo 'two: '.$e->getMessage()."\n";
  }
  try {
    echo 'full positional: '.string_default_two('x').' '.
      string_default_two('x', a = true)."\n";
  } catch (Exception $e) {
    echo 'full positional: '.$e->getMessage()."\n";
  }
  try {
    echo 'partial positional: '.partial_positional('x').' '.
      partial_positional('x', a = true)."\n";
  } catch (Exception $e) {
    echo 'partial positional: '.$e->getMessage()."\n";
  }
  try {
    echo 'vec: '.vec_default().' '.vec_default(a = true)."\n";
  } catch (Exception $e) {
    echo 'vec: '.$e->getMessage()."\n";
  }
  try {
    echo 'constructor: '.(new DefaultConstructor())->result.' '.
      (new DefaultConstructor(a = true))->result."\n";
  } catch (Exception $e) {
    echo 'constructor: '.$e->getMessage()."\n";
  }
}
