<?hh

class ShapeKeyValues {
  <<__ShapeKey>>
  const int LITERAL = 1;

  <<__ShapeKey>>
  const string STRING_LITERAL = 'literal';

  <<__ShapeKey>>
  const string LABEL = 'LABEL';

  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const int SELF_CONST = self::LITERAL;

  <<__ShapeKey>>
  const int FORWARDED_CONST = ShapeKeyValues::LITERAL;

  <<__ShapeKey>>
  const int COMPUTED = self::LITERAL + 1;

  const int UNANNOTATED = 2;
}

trait ShapeKeyTraitValues {
  <<__ShapeKey>>
  const int LITERAL = 1;

  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const int SELF_CONST = self::LITERAL;
}

interface ShapeKeyInterfaceValues {
  <<__ShapeKey>>
  const int LITERAL = 3;

  <<__ShapeKey>>
  const string LABEL = 'LABEL';

  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  const int UNANNOTATED = 4;
}

type IntShapeKeyValue = shape(ShapeKeyValues::LITERAL => int);
type StringShapeKeyValue = shape(ShapeKeyValues::STRING_LITERAL => string);
type InterfaceShapeKeyValue = shape(ShapeKeyInterfaceValues::LITERAL => int);

class :x:y {
  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const string EXPLICIT_NAME = nameof :x:y;
}
