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

class AcceptedNameKeys {
  <<__ShapeKey>>
  const classname<AcceptedNameKeys> CLASS_NAME = self::class;
  <<__ShapeKey>>
  const HH\typename<AcceptedNameKeys> TYPE_NAME = self::class;
  <<__ShapeKey>>
  const HH\enumname<int> INT_ENUM_NAME = IntKeyEnum::class;
  <<__ShapeKey>>
  const HH\enumname<string> STRING_ENUM_NAME = StringKeyEnum::class;
  <<__ShapeKey>>
  const HH\enumname<OpaqueKeyEnum> OPAQUE_ENUM_NAME = OpaqueKeyEnum::class;
  <<__ShapeKey>>
  const classname<KeyEnumClass> ENUM_CLASS_NAME = KeyEnumClass::class;
  <<__ShapeKey>>
  const string STRING = 'key';
  <<__ShapeKey>>
  const int INTEGER = 42;
  <<__ShapeKey>>
  const arraykey ARRAYKEY = 'arraykey';

  const type ClassnameKey = shape(self::CLASS_NAME => int);
  const type TypenameKey = shape(self::TYPE_NAME => int);
  const type IntEnumNameKey = shape(self::INT_ENUM_NAME => int);
  const type StringEnumNameKey = shape(self::STRING_ENUM_NAME => int);
  const type OpaqueEnumNameKey = shape(self::OPAQUE_ENUM_NAME => int);
  const type EnumClassNameKey = shape(self::ENUM_CLASS_NAME => int);
  const type StringKey = shape(self::STRING => int);
  const type IntegerKey = shape(self::INTEGER => int);
  const type ArraykeyKey = shape(self::ARRAYKEY => int);
}

function accept_classname(classname<AcceptedNameKeys> $_): void {}
function accept_enumname(HH\enumname<int> $_): void {}

function pointer_arguments(class<AcceptedNameKeys> $cls, enum<int> $enum): void {
  accept_classname($cls);
  accept_enumname($enum);
}
