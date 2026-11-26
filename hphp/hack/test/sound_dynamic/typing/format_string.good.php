<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


<<__SupportDynamicType>>
function getStrings():~vec<string> {
  return vec["ABC"];
}

<<__SupportDynamicType>>
function getInts():~vec<int> {
  return vec[42];
}

<<__SupportDynamicType>>
function getFloats():~vec<float> {
  return vec[2.3];
}

<<__SupportDynamicType>>
function getDyn():dynamic {
  return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>(2.4);
}

<<__SupportDynamicType>>
function mysprintf(
  \HH\FormatString<\PlainSprintf> $fmt,
  supportdyn<mixed> ...$fmt_args
)[]: ~string {
  return "A";
  }

<<__SupportDynamicType>>
function test(~vec<int> $v):void {
  $s = getStrings()[0];
  $i = getInts()[0];
  $f = getFloats()[0];
  $d = getDyn();
  $t = mysprintf("test %s", $s);
  echo $t;
}

<<__SupportDynamicType, __EntryPoint>>
function main():void {
  test(vec[]);
}
