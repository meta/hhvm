<?hh

function takes_string(string $_): void {}

class C {}

function c_to_mixed(C $c): mixed { return $c; }

<<__EntryPoint>>
function f(): void {
  $c = new C();
  takes_string((string) c_to_mixed($c));
}
