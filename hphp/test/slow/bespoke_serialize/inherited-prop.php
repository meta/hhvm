<?hh

// A subclass copies its parent's declPropInit when it is created. If the
// parent's property source is deserialized first, the parent's slot already
// holds the static bespoke array, and a subclass created afterwards inherits
// it. Deserializing the subclass's own source must still see a vanilla array.

class P {
  public dict<string, int> $d = dict['a' => 1, 'b' => 2, 'c' => 3];
}

class C0 extends P {}
class C1 extends P {}
class C2 extends P {}
class C3 extends P {}
class C4 extends P {}
class C5 extends P {}
class C6 extends P {}
class C7 extends P {}
class C8 extends P {}
class C9 extends P {}
class C10 extends P {}
class C11 extends P {}
class C12 extends P {}
class C13 extends P {}
class C14 extends P {}
class C15 extends P {}

function read(P $o): int {
  return $o->d['a'] + $o->d['b'] + $o->d['c'];
}

// The classes are only named through `new $cls()`, so the profiled regions
// don't refer to the subclasses and the jumpstart profile first defines them
// while deserializing their property sources.
function make(classname<P> $cls): P {
  return new $cls();
}

<<__EntryPoint>>
function main(): void {
  $classes = __hhvm_intrinsics\launder_value(vec[P::class, C0::class, C1::class, C2::class, C3::class, C4::class, C5::class, C6::class, C7::class, C8::class, C9::class, C10::class, C11::class, C12::class, C13::class, C14::class, C15::class]);
  $sum = 0;
  for ($i = 0; $i < 100; $i++) {
    foreach ($classes as $cls) {
      $sum += read(make($cls));
    }
  }
  echo $sum, "\n";
}
