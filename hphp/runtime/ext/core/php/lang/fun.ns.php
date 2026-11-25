<?hh

namespace __SystemLib {
  // systemlib can't have closures, so we get this...
  final class MethCallerHelper {
    private ?string $class;
    private ?string $method;
    public function __construct(string $class, string $method)[] {
      $this->class = $class;
      $this->method = $method;
    }
    public function __invoke($x, ...$args)[/* caller */] {
      invariant(
        \is_a($x, $this->class),
        'object must be an instance of ('.$this->class.'), instead it is ('.
        (\is_object($x) ? \get_class($x) : \gettype($x)).')'
      );
      return $x->{$this->method}(...$args);
    }
    public function getClassNameImpl()[]: string {
      return $this->class;
    }
    public function getMethodNameImpl()[]: string {
      return $this->method;
    }
    public function getClassName()[]: string {
      if (\ini_get("hhvm.notice_on_meth_caller_helper_use")) {
        \trigger_error("getClassName() called on __SystemLib\MethCallerHelper",
          \E_USER_WARNING);
      }
      return $this->getClassNameImpl();
    }
    public function getMethodName()[]: string {
      if (\ini_get("hhvm.notice_on_meth_caller_helper_use")) {
        \trigger_error(
          "getMethodName() called on __SystemLib\MethCallerHelper",
          \E_USER_WARNING);
      }
      return $this->getMethodNameImpl();
    }
  }

  final class DynMethCallerHelper {
    public function __construct(
      private string $class,
      private string $method,
      private mixed $lambda,
    )[] {
    }
    public function __invoke($x, ...$args)[/* caller */] {
      invariant(
        \is_a($x, $this->class),
        'object must be an instance of ('.$this->class.'), instead it is ('.
        (\is_object($x) ? \get_class($x) : \gettype($x)).')'
      );
      return ($this->lambda)($x, $this->method, ...$args);
    }
    public function getClassNameImpl()[]: string {
      return $this->class;
    }
    public function getMethodNameImpl()[]: string {
      return $this->method;
    }
    public function getClassName()[]: string {
      return $this->getClassNameImpl();
    }
    public function getMethodName()[]: string {
      return $this->getMethodNameImpl();
    }
  }

  function dynamic_meth_caller(string $class, string $method, mixed $lambda,
                               bool $force)[] {
    if (!$force) {
      \__SystemLib\check_dynamically_callable_inst_method($class, $method);
    }
    return new \__SystemLib\DynMethCallerHelper($class, $method, $lambda);
  }
}

namespace HH {

/**
 * Like `fun`, but with the purpose of
 * calling an instance method on any object of a certain class.
 *
 * With `fun` you'd pass in something like `'count'` and it'd call `count($x)`
 * on whatever you pass in. This, rather, will call `$x->count()` for whatever
 * _object_ `$x` you pass in, which must be of type `$class`.
 *
 * For example:
 *
 * ```
 * <?hh
 * $v = Vector {
 *   Vector {1, 2, 3},
 *   Vector {1, 2}
 * };
 * $v->map(meth_caller(Vector::class, 'count'))  // returns Vector {3, 2}
 * ```
 *
 * ...calls the `count` method on the inner vectors, and returns a vector
 * of the results of that.
 *
 * See also:
 *  - [`fun`](/hack/reference/function/HH.fun/)
 *  - [`class_meth`](/hack/reference/function/HH.class_meth/)
 *  - [`inst_meth`](/hack/reference/function/HH.inst_meth/)
 *
 * @param $class The class of the method to call. Must be a constant string.
 * @param $method The method of the class that will be called. Must be a
 *        constant string.
 * @return A callback which will call `$method` when invoked.
 *
 * @guide /hack/callables/special-functions
 */
function meth_caller(class_or_classname<mixed> $class, string $method)[] {
  // TODO(T168044199) Persist class pointer through MethCallerHelper
  return new \__SystemLib\MethCallerHelper(\HH\class_to_classname($class), $method);
}

}
