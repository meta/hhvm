<?hh

class Foo {}

// 1. Direct duplicate: two members with the same value.
enum EnumDupValues: int as int {
  Low = 1;
  Medium = 1; // duplicate value of Low
  High = 3;
}

// 2. Duplicate introduced by enum inclusion: an included member and a locally
//    declared member share a value.
enum EnumDupBase: int as int {
  A = 1;
  B = 2;
}

enum EnumDupViaUse: int as int {
  use EnumDupBase;
  C = 1; // collides with EnumDupBase::A (= 1)
}

// 3. Duplicate from including two enums whose values overlap. Each base is fine
//    on its own; the collision only exists in the enum that includes both.
enum EnumDupIncludedOne: int as int {
  X = 10;
}

enum EnumDupIncludedTwo: int as int {
  Y = 10; // same value as EnumDupIncludedOne::X
}

enum EnumDupIncludesBoth: int as int {
  use EnumDupIncludedOne;
  use EnumDupIncludedTwo;
}

// 4. String enum: single- and double-quoted literals canonicalize the same.
enum EnumDupStrings: string {
  S1 = 'dup';
  S2 = "dup"; // same value as S1
}

// 5. Two `nameof` of the same class collide. A `nameof` is intentionally not
//    matched against an equivalent string literal: its recorded value is kept
//    distinct from plain strings to avoid fragile name-resolution guesses.
enum EnumDupNameof: string {
  N1 = nameof Foo;
  N2 = nameof Foo; // same value as N1
}

// 6. No false positive: distinct values are fine.
enum EnumDistinct: int as int {
  M = 1;
  N = 2;
}

// 7. The <<__AllowUncheckedEnumValues>> attribute opts the enum out of the check,
//    so the duplicate below is permitted (no error).
<<__AllowUncheckedEnumValues>>
enum EnumDupAllowed: int as int {
  Aa = 1;
  Bb = 1; // duplicate of Aa, permitted by the attribute
}
