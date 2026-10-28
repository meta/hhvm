<?hh
// Folded decls record a value (cc_value) on each enum member whose
// initializer is checkable: int (incl. +/- literals; ints outside OCaml's
// 63-bit `int`, including i64::MIN, use the CVLargeInt slow path), string,
// and nameof. Class pointers and computed initializers record nothing.

final class C {}

enum E: arraykey {
  IntVal = 1;
  NegIntVal = -2;
  HexVal = 0x10;
  BigIntVal = 9223372036854775807;
  MinIntVal = -9223372036854775808;
  StrVal = "hello";
  NameofVal = nameof C;
  ClassConstVal = C::class;
  Computed = 1 + 1;
}
