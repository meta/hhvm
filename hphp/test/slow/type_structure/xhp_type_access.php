<?hh

class :x {
  const type T = int;
}

class Y {
  const type T = :x;
}

function f<T>(T $x): void {}
function r<reify T>(T $x): void {}

<<__EntryPoint>>
function main(): void {
  f<Y::T::T>(3);
  r<Y::T::T>(3);
  echo "done\n";
}
