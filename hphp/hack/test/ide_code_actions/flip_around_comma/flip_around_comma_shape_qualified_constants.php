<?hh

namespace ShapeKeys;

enum Keys: string {
  FIRST = 'first';
  SECOND = 'second';
}

function values(): void {
  shape(\ShapeKeys\Keys::FIRST => 1, /*range-start*//*range-end*/\ShapeKeys\Keys::SECOND => 2);
}
