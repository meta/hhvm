<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

interface IBase {}
interface IChild extends IBase {}

abstract class P {
  abstract const type T;
  <<__NeedsConcrete>>
  public static function foo(): void {
    hh_show(Shapes::idx(type_structure(static::class, 'T'), 'classname'));
  }
}

abstract class C {
  abstract const type T as IBase;
  <<__NeedsConcrete>>
  public static function foo(): void {
    hh_show(type_structure(static::class, 'T')['classname']);
  }
}

class D extends C {
  const type T = IChild;
  public static function foo(): void {
    hh_show(type_structure(static::class, 'T')['classname']);
  }
}
