<?hh

namespace ShapeKeyTest\Nested;

class Named {
  <<__ShapeKey>>
  const string NAMEOF = nameof Named;
  <<__ShapeKey>>
  const string RUNTIME_NAME = 'ShapeKeyTest\\Nested\\Named';
  <<__ShapeKey>>
  const string UNQUALIFIED_NAME = 'Named';
  <<__ShapeKey>>
  const string GLOBAL_NAME = '\\ShapeKeyTest\\Nested\\Named';
}

type NamespacedNameCollision = shape(
  nameof Named => int,
  Named::RUNTIME_NAME => string,
);

type OtherNamesAreDistinct = shape(
  Named::NAMEOF => int,
  Named::UNQUALIFIED_NAME => string,
  Named::GLOBAL_NAME => bool,
);

function namespaced_constant_collision(): void {
  $_ = shape(
    Named::NAMEOF => 1,
    Named::RUNTIME_NAME => 'duplicate',
  );
}
