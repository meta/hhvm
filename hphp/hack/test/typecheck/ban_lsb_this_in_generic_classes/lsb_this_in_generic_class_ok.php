<?hh

class NonGeneric {
  <<__LSB>> private static ?this $slot = null;
}

abstract class GenericWithTypeConst<T> {
  abstract const type TC;
  <<__LSB>> private static ?this::TC $slot = null;
}

class GenericWithIntStatic<T> {
  private static int $count = 0;
}

trait TGenericSlot<T> {
  <<__LSB>> private static ?this $slot = null;
}

class UsesGenericSlot {
  use TGenericSlot<int>;
}
