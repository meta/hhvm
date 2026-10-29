<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

interface I { }
abstract class C
{
  abstract const type TC;

  <<__NeedsConcrete>>
  public static function getClassname(
  ): ?classname<this::TC> {
    $x = type_structure(static::class, 'TC');
    $y = $x['classname'];
    return $y;
  }
}
class D extends C implements I {
  const type TC = D;
}

<<__EntryPoint>>
function main():void {
  D::getClassname();
}
