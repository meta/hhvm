<?hh

<<__ConsistentConstruct>>
class P {
  <<__NeedsConcrete>>
  public static function create(): this {
    return new static();
  }
}

class C extends P {

  <<__NeedsConcrete>>
  public static function selfWrapper(): this {
    $static = self::create();
    hh_show($static);
    return $static;
  }

  <<__NeedsConcrete>>
  public static function parentWrapper(): this {
    $static = parent::create();
    hh_show($static);
    return $static;
  }

  <<__NeedsConcrete>>
  public static function staticWrapper(): this {
    $static = static::create();
    hh_show($static);
    return $static;
  }

  public static function cWrapper(): C {
    $c = C::create();
    hh_show($c);
    return $c;
  }

  public static function pWrapper(): P {
    $p = P::create();
    hh_show($p);
    return $p;
  }
}
