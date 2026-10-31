<?hh
<<__SupportDynamicType>>
class TestClass { // 2. Rename TestClass
  const CONSTANT = 5;
  <<__ShapeKey>> const string STR_CONSTANT = "hello";
  const vec<int> VEC_INT_CONSTANT = vec [1, 2, 3, 4];
  public int $property;

  public function __construct(int $i) {
    $this->property = $i + TestClass::CONSTANT; // 1. Rename CONSTANT
  }

  public function test_method(): int {
    return $this->property;
  }

  /**
   * Some docblock
   *
   *
   */
  public function deprecated_method(int $x, string $y, int $v, mixed ...$_): int {
    return $x;
  }

  // 8. Rename depr_static
  public static function depr_static(int $x, string $y, int $v, mixed ...$_): int {
    return $x;
  }

  // 9. Rename depr_async
  public async function depr_async(int $x, int $v, mixed ...$_): Awaitable<int> {
    return $x;
  }

  // 10. Rename depr_static_async
  public static async function depr_static_async(int $x): Awaitable<
    void,
  >
  {}
}

enum RenameTestEnum: int {
  SMALL = 0;
  MEDIUM = 1;
  LARGE = 2;
}

function test_rename(): void {
  $test_class = new TestClass(1);
  // 3. Rename 1st test_method
  $test_class->test_method(); $test_class->test_method();
  $const = TestClass::CONSTANT;
  $str_const = TestClass::STR_CONSTANT; // 4. Rename STR_CONSTANT
  $vec_int_const = TestClass::VEC_INT_CONSTANT; // 5. Rename VEC_INT_CONSTANT
  // 7. Renaming deprecated_method
  $num = $test_class->deprecated_method(5, "", 4, 4, 3, 5, "hello");
  $size = RenameTestEnum::SMALL; // 13. Rename TestEnum
}

async function async_test_rename(): Awaitable<int> {
  return 5;
}

async function call_test_rename(): Awaitable<int> {
  test_rename(); // 11. Rename test_rename
  return await async_test_rename(); // 12. Rename async_test_rename
}

function test_rename_localvar(int $local): void {      //  Should match
  $local = 3;                                           //  Should match
  j($local) + $local + h("\$x = $local");     //  1st, 2nd, and 4th should match
  $lambda1 = $x ==> $x + 1;                         //  Should not match
  $lambda2 = $a ==> $local + $a; // 6. Renaming this $local    //  Should match
  $lambda3 = function($x) {                         //  Should not match
    return $x + 1; };                               //  Should not match
  $lambda4 = function($b) use($local) {                 //  Should match
    return $local + $b; };                              //  Should match
}

type RenameShapeKeyEnum =
  shape(RenameTestEnum::SMALL => int); // 4. Rename STR_CONSTANT

type RenameShapeKeyConstant =
  shape(TestClass::STR_CONSTANT => int); // 4. Rename STR_CONSTANT

function test_rename_shape_keys(): void {
  shape(RenameTestEnum::SMALL => 123); // 4. Rename STR_CONSTANT
  shape(TestClass::STR_CONSTANT => 123); // 13. Rename TestEnum
}

function multifile_rename_target(): void { // 14. Rename multifile_rename_target
}

function test_multifile_1(): void {
  multifile_rename_target();
}
