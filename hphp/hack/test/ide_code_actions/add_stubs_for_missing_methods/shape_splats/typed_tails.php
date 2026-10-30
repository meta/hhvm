<?hh

type Base = shape('base' => int);
class Payload {}

interface ShapeInterface {
  public function transform(
    shape('count' => int, string...) $input,
  ): shape(Payload...);

  public function nested(
    shape(...Base, 'child' => shape(?int...)) $input,
  ): shape('result' => shape(bool...));
}

class Child implements /*range-start*/ShapeInterface/*range-end*/ {}
