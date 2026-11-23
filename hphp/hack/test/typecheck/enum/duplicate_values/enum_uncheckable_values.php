<?hh

class Bar {
  const int C = 1;
}

// 1. Constant reference: value can't be statically checked.
enum EnumConstRef: int as int {
  A = Bar::C; // uncheckable
  B = 2;
}

// 2. Arithmetic: computed value can't be statically checked.
enum EnumArith: int as int {
  X = 1 + 1; // uncheckable
  Y = 3;
}

// 3. Several uncheckable members are reported together, on the enum.
enum EnumManyComputed: int as int {
  P = Bar::C; // uncheckable
  Q = 1 + 1; // uncheckable
}

// 4. No false positive: an all-literal enum is fully checkable, no error.
enum EnumAllLiteral: int as int {
  M = 1;
  N = 2;
}

// 5. Enum classes use a different declaration form and are not checked at all,
//    even when two members share a value.
enum class EnumClassNotChecked: int {
  int A = 1;
  int B = 1;
}

// 7. More than `max_reasons` (5) uncheckable members: the error lists the first
//    few reasons and notes how many were omitted ("(and N more)").
enum EnumManyUncheckable: int as int {
  R1 = 1 + 1; // uncheckable
  R2 = 2 + 2; // uncheckable
  R3 = 3 + 3; // uncheckable
  R4 = 4 + 4; // uncheckable
  R5 = 5 + 5; // uncheckable
  R6 = 6 + 6; // uncheckable
  R7 = 7 + 7; // uncheckable
}

class Prefixes {
  const string P = 'prefix_';
}

// 8. String concatenation of a class constant with a literal. Codegen reaches
//    for this whenever members share a prefix, so it is the most common way
//    real code becomes uncheckable, and it is not covered by the arithmetic
//    case above.
enum EnumConcatClassConst: string as string {
  PREFIXED = Prefixes::P.'suffix'; // uncheckable
  LITERAL = 'plain';
}

// 9. Concatenation onto another member of the same enum, deriving a value from
//    a sibling rather than an outside constant.
enum EnumConcatSelfConst: string as string {
  BASE = 'base';
  DERIVED = self::BASE.':extra'; // uncheckable
}
