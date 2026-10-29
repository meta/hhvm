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

function e<T>(HH\MemberOf<E, Box<T>> $param): T {
  return $param->data;
}

function generic<TEnum as E, T>(HH\MemberOf<TEnum, Box<T>> $param): T {
  return $param->data;
}

enum class F: ExBox extends E {
   Box<num> C = new Box(3.14);
}

function f<T>(HH\MemberOf<F, Box<T>> $param): T {
  return $param->data;
}

function testit3(): void {
  expect_string(e(F::A));
  expect_int(e(F::B));

  expect_string(f(E::A));
  expect_int(f(E::B));
  expect_string(f(F::A));
  expect_int(f(F::B));
  expect_num(f(F::C));

  expect_string(generic(F::A));
  expect_int(generic(F::B));
  expect_num(generic(F::C));
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
  const type TEnum = F;
}

function test_constraint(): void {
  $ext = new Ext();
  echo (string) $ext->get(E::A);
  echo "\n";
  echo (string) $ext->get(F::C);
  echo "\n";
}
