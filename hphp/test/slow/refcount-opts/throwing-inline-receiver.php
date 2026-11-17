<?hh

final class Temporary {
  <<__ALWAYS_INLINE>>
  public function outer(bool $fail): int {
    return $this->inner($fail);
  }

  <<__ALWAYS_INLINE>>
  public function inner(bool $fail): int {
    try {
      return $this->leaf($fail);
    } catch (Exception $e) {
      throw $e;
    }
  }

  <<__NEVER_INLINE>>
  public function leaf(bool $fail): int {
    if ($fail) {
      throw new Exception('expected');
    }
    return 42;
  }
}

<<__NEVER_INLINE>>
function reproduce(bool $fail): int {
  $temporary = new Temporary();
  return $temporary->outer($fail);
}

<<__EntryPoint>>
function main(): void {
  for ($i = 0; $i < 100; $i++) {
    reproduce(false);
  }
  try {
    reproduce(true);
  } catch (Exception $e) {
    echo "caught\n";
  }
}
