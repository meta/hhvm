<?hh // strict

class C {}

trait T {}

interface I {}

enum E: int {
  A = 1;
}

function takes_pointers(): void {
  $_a = new ReflectionClass(C::class);
  $_b = new ReflectionClass(T::class);
  $_c = new ReflectionClass(I::class);
  $_d = new ReflectionClass(E::class);
}

function takes_string(string $s): void {
  $_e = new ReflectionClass($s);
}

function takes_object(C $o): void {
  $_f = new ReflectionObject($o);
  $_g = new ReflectionObject(null);
}
