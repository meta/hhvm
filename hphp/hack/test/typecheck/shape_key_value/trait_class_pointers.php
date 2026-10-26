<?hh

<<file:__EnableUnstableFeatures('class_type')>>

trait TraitShapeKeyClassPointers {
  <<__ShapeKey>>
  const mixed SELF_CLASS_POINTER = self::class;

  <<__ShapeKey>>
  const class<UsesTraitShapeKeyClassPointers> EXPLICIT_CLASS_POINTER = UsesTraitShapeKeyClassPointers::class;
}

class UsesTraitShapeKeyClassPointers {
  use TraitShapeKeyClassPointers;
}
