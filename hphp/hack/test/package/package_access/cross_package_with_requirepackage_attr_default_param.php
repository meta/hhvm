//// pkg2/b.php
<?hh

class Pkg2Consts {
  const int VALUE = 2;
}

//// pkg3/c.php
<?hh

class Pkg3Consts {
  const int VALUE = 3;
}

//// a.php
<?hh
// package pkg1

class DefaultParams {
  // Defaults see the required package, like the body
  <<__RequirePackage("pkg2")>>
  public function withRequiredPackage(int $x = Pkg2Consts::VALUE): int {
    $f = (int $y = Pkg2Consts::VALUE) ==> $y;
    return $f() + Pkg2Consts::VALUE;
  }

  public function withoutRequiredPackage(
    int $x = Pkg2Consts::VALUE, // error
  ): void {}

  <<__RequirePackage("pkg2")>>
  public function outOfReach(
    int $x = Pkg3Consts::VALUE, // error
  ): void {}
}

<<__RequirePackage("pkg2")>>
function top_level_with_required_package(int $x = Pkg2Consts::VALUE): int {
  return Pkg2Consts::VALUE;
}

function top_level_without_required_package(
  int $x = Pkg2Consts::VALUE, // error
): void {}
