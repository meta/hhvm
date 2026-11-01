<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

final class BadMemoParamBox<+T> implements IMemoizeParam {
  public function getInstanceKey(): string {
    return '';
  }

  public function __construct(private T $thing) {}

  public function get()[]: T {
    return $this->thing;
  }
}

<<__Memoize(#MakeICInaccessible)>>
function bad_get_out_of_box<T>(BadMemoParamBox<T> $box): T {
  return $box->get();
}

function takes_string(string $_): void {}

<<__EntryPoint>>
function mymain(): void {
  $int_box = new BadMemoParamBox(0);
  $string_box = new BadMemoParamBox("");
  bad_get_out_of_box($int_box);
  $string_is_actually_int = bad_get_out_of_box($string_box);
  takes_string($string_is_actually_int);
}
