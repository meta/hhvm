<?hh

class ShapeKeyBase {
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
  const int FORWARDED_CONST = ShapeKeyTraitUser::LITERAL;

  <<__ShapeKey>>
  const int COMPUTED = self::LITERAL + 1;

  const int UNANNOTATED = 2;
}

class ShapeKeyChild extends ShapeKeyBase {}

trait ShapeKeyTrait {
  <<__ShapeKey>>
  const int LITERAL = 1;

  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const int SELF_CONST = self::LITERAL;
}

class ShapeKeyTraitUser {
  use ShapeKeyTrait;
}

interface ShapeKeyInterface {
  <<__ShapeKey>>
  const int LITERAL = 3;

  <<__ShapeKey>>
  const string LABEL = 'LABEL';

  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  const int UNANNOTATED = 4;
}

interface ShapeKeyInterfaceChild extends ShapeKeyInterface {}

class ShapeKeyInterfaceUser implements ShapeKeyInterfaceChild {}

class :x:y {
  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const string EXPLICIT_NAME = nameof :x:y;
}
