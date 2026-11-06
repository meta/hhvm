<?hh

enum EInt: int as int {
  ONE = 1;
}

function cast_vec<<<__Enforceable>> reify T>(mixed $x): vec<T> {
  return vec[$x as T];
}

function takes_int(int $_): void {}

<<__EntryPoint>>
function main(): void {
  takes_int(cast_vec<EInt>('1')[0]);
}
