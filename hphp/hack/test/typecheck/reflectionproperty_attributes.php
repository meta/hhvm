<?hh
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *
 */

class FooAttr implements HH\InstancePropertyAttribute {}

class Foo {
  <<FooAttr>>
  public int $bar = 1;

  public function testAttr(): void {
    (new ReflectionClass(Foo::class))->getProperty('bar')->getAttributes();
    (new ReflectionClass(Foo::class))->getProperty('bar')->getAttribute('FooAttr');
    return;
  }


}
