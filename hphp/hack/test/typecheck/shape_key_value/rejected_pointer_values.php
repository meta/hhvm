<?hh

enum IntNameEnum: int as int {
  KEY = 1;
}

enum StringNameEnum: string as string {
  KEY = 'key';
}

enum OpaqueNameEnum: int {
  KEY = 1;
}

enum class NameEnumClass: string {
  string KEY = 'key';
}

class RejectedPointerValues {
  <<__ShapeKey>>
  const class<RejectedPointerValues> CLASS_POINTER = RejectedPointerValues::class;
  <<__ShapeKey>>
  const class<RejectedPointerValues> SELF_CLASS_POINTER = self::class;
  <<__ShapeKey>>
  const classname<RejectedPointerValues> CLASSNAME_FROM_POINTER = self::class;
  <<__ShapeKey>>
  const HH\typename<RejectedPointerValues> TYPENAME_FROM_POINTER = self::class;
  <<__ShapeKey>>
  const enum<int> INT_ENUM_POINTER = IntNameEnum::class;
  <<__ShapeKey>>
  const enum<string> STRING_ENUM_POINTER = StringNameEnum::class;
  <<__ShapeKey>>
  const enum<OpaqueNameEnum> OPAQUE_ENUM_POINTER = OpaqueNameEnum::class;
  <<__ShapeKey>>
  const HH\enumname<int> INT_ENUMNAME_FROM_POINTER = IntNameEnum::class;
  <<__ShapeKey>>
  const HH\enumname<string> STRING_ENUMNAME_FROM_POINTER = StringNameEnum::class;
  <<__ShapeKey>>
  const HH\enumname<OpaqueNameEnum> OPAQUE_ENUMNAME_FROM_POINTER = OpaqueNameEnum::class;
  <<__ShapeKey>>
  const class<NameEnumClass> ENUM_CLASS_POINTER = NameEnumClass::class;
  <<__ShapeKey>>
  const classname<NameEnumClass> ENUM_CLASSNAME_FROM_POINTER = NameEnumClass::class;
  <<__ShapeKey>>
  const arraykey ARRAYKEY_FROM_POINTER = self::class;
}
