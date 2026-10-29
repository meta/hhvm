<?hh
// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.

// User code
function expect_int(int $_): void {}
function expect_string(string $_): void {}
function expect_num(num $_): void {}

interface ExBox {}

class Box<T> implements ExBox {
  public function __construct(public T $data)[] {}
}

class IBox extends Box<int> {
  public function add(int $x)[write_props]: void {
    $this->data = $this->data + $x;
  }
}

abstract final class Helper {
  public static function ibox()[]: IBox {
    return new IBox(42);
  }
}

// Enum Class user code
enum class E: ExBox {
   Box<string> A = new Box('bli');
   IBox B = Helper::ibox();
   Box<int> B2 = new Box(42);
}

enum class F: ExBox extends E {
   Box<num> C = new Box(3.14);
}

abstract class Base {
  abstract const type TEnum as E;

  public function get<T>(HH\MemberOf<this::TEnum, Box<T>> $param): T {
    return $param->data;
  }

  <<__NeedsConcrete>>
  public static function dictGen(): dict<string, ExBox> {
    $dict = dict[];
    $ts = type_structure(static::class, 'TEnum');
    $cls = $ts['classname'];
    foreach ($cls::getValues() as $key => $elt) {
      $dict[$key] = $elt;
    }
    return $dict;
  }
}

class Ext extends Base {
  const type TEnum = E; // [0]
}

function test_constraint(): void {
  $ext = new Ext();
  echo $ext->get(F::C); // F vs E, because of [0].
  echo "\n";
}
