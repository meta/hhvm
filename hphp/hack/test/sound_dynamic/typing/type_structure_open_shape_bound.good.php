<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

<<__SupportDynamicType>>
function shapeToDict(supportdyn<shape(...)> $shape): ~dict<arraykey, supportdyn<mixed>> {
  return dict[];
}
<<__SupportDynamicType>>
abstract class C {

  abstract const type TOutput as supportdyn<shape(...)>;

  <<__NeedsConcrete>>
  final public static function getOutputFields(): ~dict<arraykey,supportdyn<mixed>> {
    $x = type_structure(static::class, 'TOutput')['fields'];
    $y = shapeToDict($x);
    return $y;
}
}
