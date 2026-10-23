<?hh

enum IntKeyEnum: int as int {
  KEY = 1;
}

enum StringKeyEnum: string as string {
  KEY = 'key';
}

enum OpaqueKeyEnum: int {
  KEY = 1;
}

enum class KeyEnumClass: string {
  string KEY = 'key';
}

class RejectedPointerKeys {
  <<__ShapeKey>>
  const class<RejectedPointerKeys> CLASS_POINTER = self::class;
  <<__ShapeKey>>
  const enum<int> INT_ENUM_POINTER = IntKeyEnum::class;
  <<__ShapeKey>>
  const enum<string> STRING_ENUM_POINTER = StringKeyEnum::class;
  <<__ShapeKey>>
  const enum<OpaqueKeyEnum> OPAQUE_ENUM_POINTER = OpaqueKeyEnum::class;
  <<__ShapeKey>>
  const class<KeyEnumClass> ENUM_CLASS_POINTER = KeyEnumClass::class;
  <<__ShapeKey>>
  const HH\class_or_classname<RejectedPointerKeys> CLASS_OR_NAME = self::class;
  <<__ShapeKey>>
  const HH\class_or_classname<RejectedPointerKeys> CLASS_OR_NAME_FROM_NAMEOF = nameof self;

  const type ClassPointerKey = shape(self::CLASS_POINTER => int);
  const type IntEnumPointerKey = shape(self::INT_ENUM_POINTER => int);
  const type StringEnumPointerKey = shape(self::STRING_ENUM_POINTER => int);
  const type OpaqueEnumPointerKey = shape(self::OPAQUE_ENUM_POINTER => int);
  const type EnumClassPointerKey = shape(self::ENUM_CLASS_POINTER => int);
  const type ClassOrNameKey = shape(self::CLASS_OR_NAME => int);
  const type ClassOrNameFromNameofKey = shape(self::CLASS_OR_NAME_FROM_NAMEOF => int);
}
