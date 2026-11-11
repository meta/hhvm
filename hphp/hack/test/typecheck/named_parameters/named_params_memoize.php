<?hh
<<file:__EnableUnstableFeatures('named_parameters')>>

class Foo {
  <<__Memoize>>
  public function someMethod(named int $unused): void {}

  <<__MemoizeLSB>>
  public static function someStaticMethod(named int $unused = 1): void {}

  <<__Memoize>>
  public function positionalOnly(int $_): void {}

  public function notMemoized(named int $unused): void {}
}

<<__Memoize>>
function some_function(int $_, named int $_named = 1): void {}

function some_other_function(named int $unused): void {}
