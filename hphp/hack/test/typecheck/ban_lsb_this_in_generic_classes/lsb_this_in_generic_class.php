<?hh

class Box<T> {
  <<__LSB>> private static ?this $slot = null;
  <<__LSB>> private static vec<this> $all = vec[];
}

trait TSlot {
  <<__LSB>> private static ?this $slot = null;
}

class UsesSlot<T> {
  use TSlot;
}

class NonGenericBase {
  <<__LSB>> protected static ?this $slot = null;
}

class GenericChild<T> extends NonGenericBase {}
