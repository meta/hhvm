<?hh

class :shape-key:example-name {
  <<__ShapeKey>>
  const string NAMEOF = nameof :shape-key:example-name;
  <<__ShapeKey>>
  const string RUNTIME_NAME = 'xhp_shape_key__example_name';
  <<__ShapeKey>>
  const string SOURCE_NAME = ':shape-key:example-name';
}

type XhpNameCollision = shape(
  nameof :shape-key:example-name => int,
  :shape-key:example-name::RUNTIME_NAME => string,
);

type XhpSourceNameIsDistinct = shape(
  :shape-key:example-name::NAMEOF => int,
  :shape-key:example-name::SOURCE_NAME => string,
);

function xhp_constant_collision(): void {
  $_ = shape(
    :shape-key:example-name::NAMEOF => 1,
    :shape-key:example-name::RUNTIME_NAME => 'duplicate',
  );
}
