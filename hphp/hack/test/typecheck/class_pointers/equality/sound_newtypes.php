//// defs.php
<?hh

class P {
  public static function pm(): void {}
}
class C1 extends P {
  public static function m(): void {}
}

function expect_c1(class<C1> $_): void {}

newtype BoundedC as class<C1> = class<C1>;
newtype BoundedVec as vec<class<C1>> = vec<class<C1>>;

//// uses.php
<?hh

// Newtypes are opaque outside their file, so only their bounds are visible.
// The local can hold a string, so after the comparison it is at most a name:
// every expect_c1 / ::m() below should be an error.

function sound_bounded_newtype(string $s, BoundedC $x): void {
  if ($s === $x) {
    expect_c1($s);
  }
}

function sound_bounded_newtype_vec(vec<string> $v, BoundedVec $x): void {
  if ($v === $x) {
    expect_c1($v[0]);
  }
}
