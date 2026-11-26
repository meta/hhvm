<?hh
// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


<<__SupportDynamicType>>
interface I {
  public function bar():~Awaitable<dynamic>;
}

<<__SupportDynamicType>>
class C {
  public async function foo():Awaitable<dynamic> {
    return \HH\FIXME\UNSAFE_CAST<supportdyn<mixed>, dynamic>("A");
  }
}
<<__SupportDynamicType>>
class D extends C implements I {
  public async function foo():Awaitable<int> {
    return 5;
  }
  public async function bar():Awaitable<int> {
    return 5;
  }
}
