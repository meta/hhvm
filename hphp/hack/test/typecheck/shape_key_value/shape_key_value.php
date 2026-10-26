<?hh

<<file:__EnableUnstableFeatures('class_const_default')>>

class ValidShapeKeyValues {
  <<__ShapeKey>>
  const string STRING = 'key';
  <<__ShapeKey>>
  const string LABEL = 'LABEL';
  <<__ShapeKey>>
  const string UTF8 = 'é';
  <<__ShapeKey>>
  const int INTEGER = 42;
  <<__ShapeKey>>
  const int NEGATIVE = -42;
  <<__ShapeKey>>
  const int POSITIVE = +42;
  <<__ShapeKey>>
  const int LARGE_NEGATIVE = -9223372036854775807;
  <<__ShapeKey>>
  const int MAX_INT = 0x7fffffffffffffff;
}

abstract class AbstractShapeKeyValues {
  <<__ShapeKey>>
  const string CONCRETE = 'concrete';
  <<__ShapeKey>>
  abstract const string DEFAULTED = 'default';
  <<__ShapeKey>>
  abstract const string MISSING;
}

class InvalidShapeKeyValues {
  <<__ShapeKey>>
  const string COMPUTED = 'computed'.'value';
  <<__ShapeKey>>
  const string FORWARDED = ValidShapeKeyValues::STRING;
  <<__ShapeKey>>
  const string INVALID_UTF8 = "\xff";
}

abstract class UnmarkedValueCheck {
  const string KEY = 'unmarked';
  const string COMPUTED = 'computed'.'value';
  abstract const string DEFAULTED = 'default';
  abstract const string MISSING;
}

type UnmarkedValueCheckType = shape(UnmarkedValueCheck::KEY => int);
