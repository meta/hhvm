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

class AcceptedNameValues {
  <<__ShapeKey>>
  const classname<AcceptedNameValues> CLASS_NAME = nameof AcceptedNameValues;
  <<__ShapeKey>>
  const classname<AcceptedNameValues> SELF_NAME = nameof self;
  <<__ShapeKey>>
  const HH\typename<AcceptedNameValues> TYPE_NAME = nameof self;
  <<__ShapeKey>>
  const HH\enumname<int> INT_ENUM_NAME = nameof IntNameEnum;
  <<__ShapeKey>>
  const HH\enumname<string> STRING_ENUM_NAME = nameof StringNameEnum;
  <<__ShapeKey>>
  const HH\enumname<OpaqueNameEnum> OPAQUE_ENUM_NAME = nameof OpaqueNameEnum;
  <<__ShapeKey>>
  const classname<NameEnumClass> ENUM_CLASS_NAME = nameof NameEnumClass;
}
