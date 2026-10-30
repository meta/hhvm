<?hh

type Base = shape('base' => int);
class Payload {}
interface Marker {}

abstract class ShapeParent<T> {
  abstract public function transform(
    shape(int...) $empty,
    shape('flag' => bool, ?int...) $nullable,
    shape(T...) $generic,
    shape(Payload...) $named,
    shape('child' => shape(string...)) $nested,
    shape(...Base, 'flag' => bool, int...) $splat,
    shape() $closed,
    shape('flag' => bool, ...) $open,
    shape((int | string)...) $union,
    shape((Payload & Marker)...) $intersection,
  ): shape('result' => bool, int...);
}

class Child<T> extends /*range-start*/ShapeParent/*range-end*/<T> {}
