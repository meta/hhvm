<?hh
// Copyright 2004-present Facebook. All Rights Reserved.

// The docblock of `HH\Lib\Str\search()`, `search_ci()`, and `search_last()`
// promises "If the offset is out-of-bounds, a ViolationException will be
// thrown", unconditionally. The byte/libc locale backend used to short-circuit
// empty needles and empty haystacks to -1 before validating the offset, so an
// out-of-bounds offset was silently ignored exactly when the needle or the
// haystack was empty - unlike the ICU backend, which validates unconditionally
// (`hsl_locale_icu_ops.cpp`), and unlike `Str\contains()`, which has always
// validated. These tests pin the validation behavior across all empty-input
// combinations.

use namespace HH\Lib\{Str, _Private\_Locale, _Private\_Str};

function report_int(string $label, (function(): int) $fn): void {
  try {
    printf("%s => %d\n", $label, $fn());
  } catch (\InvalidArgumentException $e) {
    printf("%s => throw: %s\n", $label, $e->getMessage());
  }
}

function report_nullable(string $label, (function(): ?int) $fn): void {
  try {
    $result = $fn();
    printf("%s => %s\n", $label, $result === null ? 'null' : (string)$result);
  } catch (\InvalidArgumentException $e) {
    printf("%s => throw: %s\n", $label, $e->getMessage());
  }
}

<<__EntryPoint>>
function main(): void {
  $c = _Locale\get_c_locale();

  // Out-of-bounds offsets must throw even when the needle or the haystack is
  // empty.
  report_int(
    "_Str\\strpos_l('foo', '', 5)",
    () ==> _Str\strpos_l('foo', '', 5, $c));
  report_int(
    "_Str\\strpos_l('foo', '', -5)",
    () ==> _Str\strpos_l('foo', '', -5, $c));
  report_int(
    "_Str\\strpos_l('', 'foo', 5)",
    () ==> _Str\strpos_l('', 'foo', 5, $c));
  report_int(
    "_Str\\stripos_l('foo', '', 5)",
    () ==> _Str\stripos_l('foo', '', 5, $c));
  report_int(
    "_Str\\strrpos_l('foo', '', 5)",
    () ==> _Str\strrpos_l('foo', '', 5, $c));
  report_int(
    "_Str\\strripos_l('foo', '', 5)",
    () ==> _Str\strripos_l('foo', '', 5, $c));
  report_int(
    "_Str\\strripos_l('', '', 5)",
    () ==> _Str\strripos_l('', '', 5, $c));

  // In-range offsets, including `offset === length` and empty inputs, keep
  // the existing "not found" result.
  report_int(
    "_Str\\strpos_l('foo', '', 3)",
    () ==> _Str\strpos_l('foo', '', 3, $c));
  report_int(
    "_Str\\strpos_l('', 'foo', 0)",
    () ==> _Str\strpos_l('', 'foo', 0, $c));
  report_int(
    "_Str\\strrpos_l('foo', '', -3)",
    () ==> _Str\strrpos_l('foo', '', -3, $c));

  // The public HSL functions share the same backend and contract.
  report_nullable(
    "Str\\search('foo', '', 5)",
    () ==> Str\search('foo', '', 5));
  report_nullable(
    "Str\\search_ci('foo', '', 5)",
    () ==> Str\search_ci('foo', '', 5));
  report_nullable(
    "Str\\search_last('foo', '', 5)",
    () ==> Str\search_last('foo', '', 5));
  report_nullable(
    "Str\\search('foo', '', 3)",
    () ==> Str\search('foo', '', 3));
  report_nullable(
    "Str\\search('foo', 'f', 5)",
    () ==> Str\search('foo', 'f', 5));
}
