<?hh

trait TraitShapeKeyValues {
  <<__ShapeKey>>
  const string SELF_NAME = nameof self;

  <<__ShapeKey>>
  const string EXPLICIT_NAME = nameof UsesTraitShapeKeyValues;

  <<__ShapeKey>>
  const string LITERAL = 'key';

  const string UNMARKED_SELF_NAME = nameof self;

  public static function selfName(): string {
    return nameof self;
  }
}

class UsesTraitShapeKeyValues {
  use TraitShapeKeyValues;
}
