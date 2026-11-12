<?hh

/**
 * ( excerpt from
 * http://php.net/manual/en/class.reflectionfunctionabstract.php )
 *
 * A parent class to ReflectionFunction and ParentMethod. Read their
 * descriptions for details.
 */
<<__NativeData>>
abstract class ReflectionFunctionAbstract implements Reflector {

  const IS_STATIC    = 1;
  const IS_PUBLIC    = 256;
  const IS_PROTECTED = 512;
  const IS_PRIVATE   = 1024;
  const IS_ABSTRACT  = 2;
  const IS_FINAL     = 4;

  /**
   * (excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getname.php )
   *
   * Get the name of the function. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     string   The name of the function.
   */
  <<__Native>>
  public function getName()[]: string;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionfunctionabstract.innamespace.php
   * )
   *
   * Checks whether a function is defined in a namespace.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  public function inNamespace()[]: bool {
    return strrpos($this->getName(), '\\') !== false;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getnamespacename.php )
   *
   * Get the namespace name where the function is defined.
   *
   * @return     string   The namespace name.
   */
  public function getNamespaceName()[]: string {
    $name = $this->getName();
    $pos = strrpos($name, '\\');
    return ($pos === false) ? '' : substr($name, 0, $pos);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getshortname.php )
   *
   * Get the short name of the function (without the namespace part).
   *
   * @return     string  The short name of the function.
   */
  public function getShortName()[]: string {
    $name = $this->getName();
    $pos = strrpos($name, '\\');
    return ($pos === false) ? $name : substr($name, $pos + 1);
  }

  <<__Native>>
  public function isHack()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.isinternal.php )
   *
   * Checks whether the function is internal, as opposed to user-defined.
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     mixed   TRUE if it's internal, otherwise FALSE
   */
  <<__Native>>
  public function isInternal()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.isclosure.php )
   *
   * Checks whether it's a closure. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     bool   TRUE if it's a closure, otherwise FALSE
   */
  public function isClosure()[]: bool {
    return false;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.isgenerator.php )
   *
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     bool   TRUE if the function is generator, otherwise FALSE.
   */
  <<__Native>>
  public function isGenerator()[]: bool;

  /**
   * @return     bool   TRUE if the function is async, otherwise FALSE.
   */
  <<__Native>>
  public function isAsync()[]: bool;

  /**
   * @return     bool   TRUE if the function is internal, otherwise FALSE.
   */
  <<__Native>>
  public function isInternalToModule()[]: bool;

  /**
   * Indicates whether the function has ...$varargs as its last parameter
   * to capture variadic arguments.
   *
   * @return     bool   TRUE if the function is variadic, otherwise FALSE
   */
  <<__Native>>
  public function isVariadic()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.isuserdefined.php )
   *
   * Checks whether the function is user-defined, as opposed to internal.
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     mixed   TRUE if it's user-defined, otherwise false;
   */
  public function isUserDefined()[]: bool {
    return !$this->isInternal();
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getfilename.php )
   *
   * Gets the file name from a user-defined function. Warning: This function
   * is currently not documented; only its argument list is available.
   *
   * Warning: if this is a method defined in a trait, getFileName() might return
   * a filename of the class using the trait instead of a filename of the trait,
   * if the trait was flattened (e.g. in repo mode by hhbbc).
   *
   * @return     mixed   The file name.
   */
  <<__Native>>
  public function getFileName()[]: mixed;

  /**
   * Gets the declaring file for a user-defined function.
   *
   * @return ReflectionFile   A ReflectionFile object of the file that the
   *                           reflected function is part of.
   */
  public function getFile()[]: ReflectionFile {
    $fileName = $this->getFileName();

    if ($fileName === false) {
      throw new ReflectionException(
        'Couldn\'t get ReflectionFile because the function was not defined in '.
        'a file.'
      );
    }

    if (!is_string($fileName)) {
      throw new ReflectionException(
        'Unexpected non-string file name for ReflectionFunction.'
      );
    }

    return new ReflectionFile((string)$fileName);
  }

  /**
   * Extract type var names used in a function's parameter type hints.
   *
   * NB: Please use getTypeVarNames() instead, as it returns all the
   * type vars that are defined on the function irrespective of how
   * they are used (params, return type, within the body etc.).
   *
   * @return keyset containing type var names as strings
   */
  <<__Native>>
  public function getParamTypeVarNames()[]: keyset<string>;

  /**
   * Get names of type vars defined on this function or method.
   *
   * @return vec containing type var names as strings
   */
  <<__Native>>
  public function getTypeVarNames()[]: vec<string>;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getstartline.php )
   *
   * Gets the starting line number of the function. Warning: This function
   * is currently not documented; only its argument list is available.
   *
   * @return     mixed   The starting line number.
   */
  <<__Native>>
  public function getStartLine()[]: mixed;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getendline.php )
   *
   * Get the ending line number. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     mixed   The ending line number of the user defined function,
   *                     or FALSE if unknown.
   */
  <<__Native>>
  public function getEndLine()[]: mixed;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getdoccomment.php )
   *
   * Get a Doc comment from a function. Warning: This function is currently
   * not documented; only its argument list is available.
   *
   * @return     mixed   The doc comment string if it exists, otherwise FALSE
   */
  <<__Native>>
  public function getDocComment()[]: mixed;

  <<__Native>>
  private function getRetTypeInfo()[]: dict;

  <<__Native>>
  private function getReturnTypeHint()[]: ?string;

  public function getReturnTypeText()[] {
    return $this->getReturnTypeHint() ?: false;
  }

  <<__Native>>
  public function getReifiedTypeParamInfo()[]: varray;

  <<__Native>>
  public function getCoeffects()[]: vec;

  /*
   * Returns the module associated with the given function.
   */
  <<__Native>>
  public function getModule()[]: ?string;

  <<__Native>>
  public function returnsReadonly()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.hasreturntype.php
   * )
   *
   * Checks if the function has a specified return type.
   *
   * @return - true if the function has a specified return type; false
   *           otherwise.
   */
  public function hasReturnType()[]: bool {
    return (bool) $this->getReturnTypeText();
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getReturnType.php
   * )
   *
   * Gets the specified return type of a function
   *
   * @return - a ReflectionType object if a return type is specified; null
   *           otherwise.
   */
  public function getReturnType()[]: ?ReflectionType {
    if ($this->hasReturnType()) {
      $retTypeInfo = $this->getRetTypeInfo();
      $name = $retTypeInfo['type_hint'];
      return new ReflectionType(
        $this,
        dict[
          'name' => $name,
          'nullable' => $retTypeInfo['type_hint_nullable'],
        ]
      );
    }
    return null;
  }

  /**
   * ( excerpt from
   *   http://php.net/manual/en/reflectionclass.getattributes.php )
   *
   * Gets all attributes
   *
   * @return  array<arraykey, array<mixed>>
   */
  <<__Native>>
  final public function getAttributesNamespaced()[]: darray<arraykey, varray<mixed>>;

  use ReflectionLegacyAttribute;

  <<__Native>>
  public function getNumberOfParameters()[]: int;

  <<__Native>>
  private function getParamInfo()[]: varray<darray<string, mixed>>;


  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getparameters.php )
   *
   * Get the parameters as an array of ReflectionParameter. Warning: This
   * function is currently not documented; only its argument list is
   * available.
   *
   * @return     array  The parameters, as a ReflectionParameter object.
   */
  <<__Memoize>>
  public function getParameters()[]: varray<ReflectionParameter> {
    // FIXME: ReflectionParameter sh/could have native data pointing to the
    // relevant Func::ParamInfo data structure
    $ret = vec[];
    foreach ($this->getParamInfo() as $idx => $info) {
      $param = new ReflectionParameter(null, null, $info);
      $ret[] = $param;
    }
    return $ret;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getnumberofrequiredparameters.php
   * )
   *
   * Get the number of required parameters that a function defines. Warning:
   * This function is currently not documented; only its argument list is
   * available.
   *
   * @return     mixed   The number of required parameters.
   */
  public function getNumberOfRequiredParameters()[] {
    $count = 0;
    $params = $this->getParameters();
    foreach ($params as $name => $param) {
      if ($param->isOptional()) {
        break;
      }
      $count++;
    }
    return $count;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.isdeprecated.php
   * )
   *
   * Returns whether the function is deprecated.
   */
  public function isDeprecated()[]: bool {
    return null !== $this->getAttribute('__Deprecated');
  }

  public function getExtension()[] {
    // FIXME: HHVM doesn't support this
    return null;
  }

  public function getExtensionName()[] {
    return null;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getclosurescopeclass.php
   * )
   *
   * Returns the scope associated to the closure
   *
   * @return     mixed   Returns the class on success or NULL on failure.
   */
  public function getClosureScopeClass()[]: ?ReflectionClass {
    return null;
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ' . get_class($this)
    );
  }

  // Implementation of __toString
  final protected function __toStringHelper(
    $type,
    varray<string> $preAttrs = vec[],
    varray<string> $funcAttrs = vec[],
  )[]: string {
    $ret = '';
    $doc = $this->getDocComment();
    if ($doc) {
      $ret .= $doc . "\n";
    }
    $ret .= "$type [ <";
    if ($this->isInternal()) {
      $ret .= 'internal';
      if ($this->isDeprecated()) {
        $ret .= ', deprecated';
      }
      $extensionName = $this->getExtensionName();
      if ($extensionName) {
        $ret .= ':' . $extensionName;
      }
    } else {
      $ret .= 'user';
    }
    if ($preAttrs) {
      $ret .= ', ' . implode(', ', $preAttrs);
    }
    $ret .= '> ';
    if ($funcAttrs) {
      $ret .= implode(' ', $funcAttrs) . ' ';
    }
    $ret .= ($type == 'Method') ? 'method ' : 'function ';
    $ret .= $this->getName() . " ] {\n";

    if ((int)$this->getStartLine() > 0) {
      $file = (string)$this->getFileName();
      $start = (string)$this->getStartLine();
      $end = (string)$this->getEndLine();
      $ret .= "  @@ $file $start - $end\n";
    }

    if ($this->isClosure()) {
      // TODO: Not enough info
    }

    $params = $this->getParameters();
    if (count($params) > 0) {
      $ret .= "\n  - Parameters [" . count($params) . "] {\n  ";
      foreach ($params as $param) {
        $ret .= '  '.str_replace("\n", "\n  ", $param->toString()."\n");
      }
      $ret .= "}\n";
    }

    $ret .= "}\n";
    return $ret;
  }
}


/**
 * ( excerpt from http://php.net/manual/en/class.reflectionfunction.php )
 *
 * The ReflectionFunction class reports information about a function.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
class ReflectionFunction extends ReflectionFunctionAbstract {

  public string $name; // should be readonly (PHP compatibility)
  private ?Closure $closure = null;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionfunction.construct.php
   * )
   *
   * Constructs a ReflectionFunction object.
   *
   * @name       mixed   The name of the function to reflect or a closure.
   *
   * @return     mixed   No value is returned.
   */
  public function __construct($name_or_closure)[] {
    if ($name_or_closure is Closure) {
      $this->closure = $name_or_closure;
      $this->__initClosure($name_or_closure);
    } else if (is_string($name_or_closure)){
      if (!$this->__initName($name_or_closure)) {
        throw new ReflectionException(
          "Function $name_or_closure does not exist");
      }
    } else {
      throw new ReflectionException(
        sprintf('%s expects a string or a Closure, got %s',
                __METHOD__, gettype($name_or_closure))
      );
    }

    $this->name = $this->getName();
  }

  <<__Native>>
  private function __initClosure(\HH\object $closure)[write_this_props]: bool;

  <<__Native>>
  private function __initName(string $name)[write_this_props]: bool;

  /**
   * (excerpt from
   * http://php.net/manual/en/reflectionfunctionabstract.getname.php )
   *
   * Get the name of the function. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     string   The name of the function.
   */
  public function getName()[]: string {
    if ($this->closure) {
      // Format: Closure$scope;hash
      $cls = get_class($this->closure);
      $ns_end = strrpos($cls, '\\');
      if ($ns_end !== false) {
        $ns_start = strpos($cls, '$') + 1;
        $ns = substr($cls, $ns_start, $ns_end - $ns_start);
        return $ns.'\\{closure}';
      }
      return '{closure}';
    }
    return parent::getName();
  }

  public function getClosure() {
    return $this->closure ?:
      function(...$args) { return $this->invokeArgs($args); };
  }

  public function isClosure()[]: bool {
    return (bool) $this->closure;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionfunction.tostring.php )
   *
   * @return     string  A representation of this ReflectionFunction.
   */
  public function __toString()[]: string {
    return $this->__toStringHelper($this->isClosure() ? 'Closure' : 'Function');
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionfunction.export.php )
   *
   * Exports a Reflected function.
   *
   * @name       mixed   The reflection to export.
   * @ret        mixed   Setting to TRUE will return the export, as opposed
   *                     to emitting it. Setting to FALSE (the default) will
   *                     do the opposite.
   *
   * @return     mixed   If the return parameter is set to TRUE, then the
   *                     export is returned as a string, otherwise NULL is
   *                     returned.
   */
  public static function export($name, $ret=false) {
    $obj = new self($name);
    $str = (string) $obj;
    if ($ret) {
      return $str;
    }
    print $str;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionfunction.invoke.php )
   *
   * Invokes a reflected function.
   *
   * @return     mixed   Returns the result of the invoked function call.
   */
  public function invoke(...$args) {
    if ($this->closure) {
      return hphp_invoke_method($this->closure, get_class($this->closure),
                                '__invoke', $args);
    }
    throw new ReflectionException(__FUNCTION__."() on top-level functions is no longer supported");
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunction.invokeargs.php )
   *
   * Invokes the function and pass its arguments as array.
   *
   * @args       mixed   The passed arguments to the function as an array,
   *                     much like call_user_func_array() works.
   *
   * @return     mixed   Returns the result of the invoked function
   */
  public function invokeArgs($args) {
    if ($this->closure) {
      return hphp_invoke_method($this->closure, get_class($this->closure),
                                '__invoke', array_values($args));
    }
    throw new ReflectionException(__FUNCTION__."() on top-level functions is no longer supported");
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionfunction.isdisabled.php )
   *
   * Checks if the function is disabled, via the disable_functions directive.
   *
   * @return     bool   TRUE if it's disable, otherwise FALSE
   */
  public function isDisabled()[]: bool {
    // FIXME: HHVM doesn't support the disable_functions directive.
    return false;
  }

  <<__Native>>
  private function getClosureScopeClassname(\HH\object $closure)[]: ?string;

  public function getClosureScopeClass(): ?ReflectionClass {
    if ($this->closure) {
      $cls = $this->getClosureScopeClassname($this->closure);
      if ($cls) {
        return new ReflectionClass($cls);
      }
    }
    return null;
  }

  <<__Native>>
  private function getClosureThisObject(\HH\object $closure): ?\HH\object;

  /**
   * Returns this pointer bound to closure.
   *
   * @return object|NULL Returns $this pointer. Returns NULL in case of
   * an error.
   */
  public function getClosureThis(): mixed {
    if ($this->closure) {
      return $this->getClosureThisObject($this->closure);
    }
    return null;
  }

  use ReflectionTypedAttribute;
}

/**
 * ( excerpt from http://php.net/manual/en/class.reflectionmethod.php )
 *
 * The ReflectionMethod class reports information about a method.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
class ReflectionMethod extends ReflectionFunctionAbstract {

  public string $name; // should be readonly (PHP compatibility)
  public string $class; // should be readonly (PHP compatibility)

  private /*string*/ $originalClass;
  private /*bool*/ $forcedAccessible = false;

  <<__Native>>
  private function __init(mixed $cls_or_obj, string $meth)[write_this_props]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.construct.php )
   *
   * Constructs a new ReflectionMethod.
   *
   * Format 1:
   *
   * @cls        mixed   Classname or object (instance of the class) that
   *                     contains the method.
   * @name       string  Name of the method.
   *
   * Format 2:
   *
   * @class_and_method string  Class name and method, separated by ::
   */
  public function __construct(...$args)[] {
    if (count($args) == 0 || count($args) > 2) {
      throw new Exception(
        'ReflectionMethod::__construct() takes either 1 or 2 arguments');
    }

    if (count($args) == 1) {
      $arr = explode('::', $args[0], 3);
      if (count($arr) !== 2) {
        $name = $args[0];
        throw new ReflectionException((string)$name." is not a valid method name");
      }
      list($cls, $name) = $arr;
      $classname = $cls;
    } else {
      $cls = $args[0];
      $mth_names = explode('::', (string)$args[1]);
      $name = $mth_names[count($mth_names) - 1];

      $classname = is_object($cls) ? get_class($cls) : $cls;
      $method = $args[1];
    }

    $this->originalClass = $classname;
    if (!$this->__init($cls, (string) $name)) {
      throw new ReflectionException(
        "Method ".(string)$classname."::".(string)$name."() does not exist");
    }

    $this->name = $this->getName();
    $this->class = $this->getDeclaringClassname();
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.tostring.php )
   *
   * @return     string  A string representation of this ReflectionMethod.
   */
  public function __toString()[]: string {
    $preAttrs = vec[];

    $decl_class = $this->getDeclaringClassname();
    if ($this->originalClass !== $decl_class) {
      $preAttrs[] = "inherits $decl_class";
    } else {
      $proto_cls = $this->getPrototypeClassname();
      if ($proto_cls) {
        $preAttrs[] = interface_exists($proto_cls, false)
          ? 'implements '.$proto_cls
          : 'overrides '.$proto_cls;
      }
    }

    if ($this->isConstructor()) {
      $preAttrs[] = 'ctor';
    }

    $funcAttrs = vec[];
    if ($this->isAbstract()) {
      $funcAttrs[] = 'abstract';
    }
    if ($this->isFinal()) {
      $funcAttrs[] = 'final';
    }
    if ($this->isStatic()) {
      $funcAttrs[] = 'static';
    }

    if ($this->isPrivate()) {
      $funcAttrs[] = 'private';
    } else if ($this->isProtected()) {
      $funcAttrs[] = 'protected';
    } else {
      $funcAttrs[] = 'public';
    }

    return $this->__toStringHelper('Method', $preAttrs, $funcAttrs);
  }

  public function __debugInfo() {
    return dict['name' => $this->name, 'class' => $this->class];
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.export.php )
   *
   * Exports a ReflectionMethod. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @cls        mixed   The class name.
   * @name       mixed   The name of the method.
   * @ret        mixed   Setting to TRUE will return the export, as opposed
   *                     to emitting it. Setting to FALSE (the default) will
   *                     do the opposite.
   *
   * @return     ?string If the return parameter is set to TRUE, then the
   *                     export is returned as a string, otherwise NULL is
   *                     returned.
   */
  public static function export(
    string $cls,
    string $name,
    bool $ret = false,
  ): ?string {
    $meth = new self($cls, $name);
    $str = (string) $meth;
    if ($ret) {
      return $str;
    }
    print $str;
  }

  private function isAccessible(): bool {
    return $this->forcedAccessible || $this->isPublic();
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.invoke.php )
   *
   * Invokes a reflected method.
   *
   * @obj        mixed   The object to invoke the method on. For static
   *                     methods, pass null to this parameter.
   *
   * @return     mixed   Returns the method result.
   */
  public function invoke($obj, ...$args): mixed {
    $this->validateInvokeParameters($obj, $args);
    if ($this->isStaticInPrologue()) {
      // Docs says to pass null, but Zend completely ignores the argument
      $obj = null;
    }
    return hphp_invoke_method($obj, $this->originalClass,
                              $this->getName(), $args);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.invokeargs.php
   * )
   *
   * Invokes the reflected method and pass its arguments as array.
   *
   * @obj        mixed   The object to invoke the method on. In case of
   *                     static methods, you can pass null to this parameter.
   * @args       mixed   The parameters to be passed to the function, as an
   *                     array.
   *
   * @return     mixed   Returns the method result.
   */
  public function invokeArgs($obj, $args): mixed {
    $this->validateInvokeParameters($obj, $args);
    if ($this->isStaticInPrologue()) {
      $obj = null;
    }
    return hphp_invoke_method($obj, $this->originalClass, $this->getName(),
                              array_values($args));
  }

  private function validateInvokeParameters($obj, $args): mixed {
    if (!$this->isAccessible()) {
      throw new ReflectionException(
        sprintf(
          'Trying to invoke %s method %s::%s() from scope ReflectionMethod',
          ($this->isProtected() ? 'protected' : 'private'),
          $this->getDeclaringClassname(), $this->getName(),
        )
      );
    }

    if (!$this->isStaticInPrologue()) {
      if (!$obj) {
        $name = $this->originalClass.'::'.$this->getName();
        throw new ReflectionException(
          "Trying to invoke non static method $name() without an object",
        );
      }
    }
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.isfinal.php )
   *
   * Checks if the method is final.
   *
   * @return     bool   TRUE if the method is final, otherwise FALSE
   */
  <<__Native>>
  public function isFinal()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.isabstract.php
   * )
   *
   * Checks if the method is abstract.
   *
   * @return     bool   TRUE if the method is abstract, otherwise FALSE
   */
  <<__Native>>
  public function isAbstract()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.ispublic.php )
   *
   * Checks if the method is public.
   *
   * @return     bool   TRUE if the method is public, otherwise FALSE
   */
  <<__Native>>
  public function isPublic()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.isprotected.php
   * )
   *
   * Checks if the method is protected.
   *
   * @return     bool   TRUE if the method is protected, otherwise FALSE
   */
  <<__Native>>
  public function isProtected()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.isprivate.php )
   *
   * Checks if the method is private. Warning: This function is currently
   * not documented; only its argument list is available.
   *
   * @return     bool   TRUE if the method is private, otherwise FALSE
   */
  <<__Native>>
  public function isPrivate()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.isstatic.php )
   *
   * Checks if the method is static.
   *
   * @return     bool   TRUE if the method is static, otherwise FALSE
   */
  <<__Native>>
  public function isStatic()[]: bool;

  <<__Native>>
  private function isStaticInPrologue()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionmethod.isconstructor.php )
   *
   * Checks if the method is a constructor.
   *
   * @return     bool   TRUE if the method is a constructor, otherwise FALSE
   */
  <<__Native>>
  public function isConstructor()[]: bool;

  <<__Native>>
  public function isReadonly()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionmethod.getmodifiers.php )
   *
   * Returns a bitfield of the access modifiers for this method.
   *
   * @return     int   A numeric representation of the modifiers. The
   *                   modifiers are listed below. The actual meanings of
   *                   these modifiers are described in the predefined
   *                   constants.
   */
  <<__Native>>
  public function getModifiers()[]: int;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionmethod.getprototype.php )
   *
   * Returns the methods prototype.
   *
   * @return     object   A ReflectionMethod instance of the method prototype.
   */
  public function getPrototype()[]: ReflectionMethod {
    $proto_cls = $this->getPrototypeClassname();
    $name = $this->getName();
    if ($proto_cls) {
      return new ReflectionMethod($proto_cls, $name);
    }
    // This message is arguably misleading without $this->class
    throw new ReflectionException(
      sprintf('Method %s::%s does not have a prototype',
              $this->originalClass,
              $name,)
    );
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionmethod.getclosure.php
   * )
   *
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @object    object  Forbidden for static methods, required for other methods
   *
   * @return    mixed   Returns Closure. Returns NULL in case of an error.
   */
  public function getClosure($object = null): ?Closure {
    if ($this->isStaticInPrologue()) {
      $object = null;
    } else {
      if (!$object) {
        trigger_error(
          'ReflectionMethod::getClosure() expects parameter 1'
          . ' to be object, ' . gettype($object) . ' given', E_WARNING);
        return null;
      }
      $cls_name = $this->getDeclaringClassname();
      if (!is_a($object, $cls_name)) {
        throw new ReflectionException(
          'Given object is not an instance of the class this method was '.
          'declared in' // mention declaringClassname / originalClass here ?
        );
      }
    }
    return function (...$args) use ($object) {
      return $this->invokeArgs($object, $args);
    };
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionmethod.setaccessible.php )
   *
   * Sets a method to be accessible. For example, it may allow protected and
   * private methods to be invoked.
   *
   * @accessible mixed   TRUE to allow accessibility, or FALSE.
   *
   * @return     mixed   No value is returned.
   */
  public function setAccessible(bool $accessible)[write_props]: void {
    // Public methods are always accessible. Cannot manually set to not be
    // accessible.
    if ($this->isPublic()) { return; }
    $this->forcedAccessible = $accessible;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionmethod.getdeclaringclass.php )
   *
   * Gets the declaring class for the reflected method.
   *
   * @return ReflectionClass   A ReflectionClass object of the class that the
   *                           reflected method is part of.
   */
  public function getDeclaringClass()[]: ReflectionClass {
    return new ReflectionClass($this->getDeclaringClassname());
  }

  public function getOriginalClassname()[]: string {
    return (string)$this->originalClass;
  }

  /**
   * Gets the "canonical" version of the method, where canonical means that
   * the method is *actually* defined and implemented on the class that is
   * within the returned value's ->getDeclaringClass()->getName().
   * This is particularly relevant when the method is inherited from a trait
   *
   * NOTE: This wouldn't work "correctly" in repo-mode, where traits are
   * flattened. As such, in repo-mode this throws an exception instead.
   */
  public function getCanonicalMethod()[]: ReflectionMethod {
    $canonical_clsname = $this->getCanonicalClassname();
    if ($canonical_clsname === $this->class) {
      return $this;
    }
    return new ReflectionMethod($canonical_clsname, $this->name);
  }

  /**
   * Gets the name of the "canonical" class, where canonical means that
   * the method is *actually* defined and implemented on that class.
   * This is particularly relevant when the method is inherited from a trait
   *
   * NOTE: This wouldn't work "correctly" in repo-mode, where traits are
   * flattened. As such, in repo-mode this throws an exception instead.
   */
  <<__Native>>
  public function getCanonicalClassname()[]: ?string;

  <<__Native>>
  private function getDeclaringClassname()[]: ?string;

  <<__Native>>
  private function getPrototypeClassname()[]: ?string; // ?string

  use ReflectionTypedAttribute;
}

/**
 * ( excerpt from http://php.net/manual/en/class.reflectionclass.php )
 *
 * The ReflectionClass class reports information about a class.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
<<__NativeData>>
class ReflectionClass implements Reflector {
  const int IS_IMPLICIT_ABSTRACT = 16;
  const int IS_EXPLICIT_ABSTRACT = 32;
  const int IS_FINAL = 64;

  public string $name; // should be readonly (PHP compatibility)
  private $obj = null;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.construct.php )
   *
   * Constructs a new ReflectionClass object. Warning: This function is
   * currently not documented; only its argument list is available.
   *
   * @name       mixed   Either a string containing the name of the class to
   *                     reflect, or an object.
   */
  public function __construct(mixed $name_or_obj)[] {
    if (HH\is_class($name_or_obj)) {
      $this->__initFromClass($name_or_obj);
      $this->name = HH\class_to_classname($name_or_obj);
      return;
    }
    if (is_object($name_or_obj)) {
      $this->obj = $name_or_obj;
      $classname = get_class($name_or_obj);
    } else {
      $classname = $name_or_obj;
    }
    $cls = $this->__init($classname);
    if ($cls is null) {
      throw new ReflectionException("Class $classname does not exist");
    }
    $this->name = HH\class_to_classname($cls);
  }

  <<__Native("NoRecording")>>
  private function __init(string $name)[]: ?class<mixed>;

  <<__Native("NoRecording")>>
  private function __initFromClass(class<mixed> $cls)[]: void;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.tostring.php )
   *
   * @return     string  A string representation of this ReflectionClass.
   */
  public function __toString()[]: string {
    $ret = '';
    $docComment = $this->getDocComment();
    if ($docComment) {
      $ret .= $docComment . "\n";
    }
    if ($this is ReflectionObject) {
      $ret .= 'Object of class [ ';
    } else if ($this->isInterface()) {
      $ret .= 'Interface [ ';
    } else if ($this->isTrait()) {
      $ret .= 'Trait [ ';
    } else if ($this->isEnum()) {
      $ret .= 'Enum [ ';
    } else {
      $ret .= 'Class [ ';
    }
    if ($this->isInternal()) {
      $ret .= '<internal:';
      $extensionName = $this->getExtensionName();
      if ($extensionName) {
        $ret .= ':' . $extensionName;
      }
      $ret .= '> ';
    } else {
      $ret .= '<user> ';
    }
    if ($this->isIterateable()) {
      $ret .= '<iterable> ';
    }
    if ($this->isInterface()) {
      $ret .= 'interface ';
    } else if ($this->isTrait()) {
      $ret .= 'trait ';
    } else if ($this->isEnum()) {
      $ret .= 'enum ';
    } else {
      if ($this->isAbstract()) {
        $ret .= 'abstract ';
      }
      if ($this->isFinal()) {
        $ret .= 'final ';
      }
      $ret .= 'class ';
    }
    $ret .= $this->getName();
    $parent = $this->getParent();
    if ($parent !== null) {
      $ret .= " extends ".HH\class_to_classname($parent);
    }
    $ifaces = $this->getInterfaceNames();
    if ($ifaces) {
      if ($this->isInterface()) {
        $ret .= ' extends ';
      } else {
        $ret .= ' implements ';
      }
      $ret .= implode(', ', $ifaces);
    }
    $ret .= " ] {\n";
    if ((int)$this->getStartLine() > 0) {
      $file = (string)$this->getFileName();
      $start = (string)$this->getStartLine();
      $end = (string)$this->getEndLine();
      $ret .= "  @@ $file $start-$end\n";
    }

    $consts = $this->getConstants();
    $abs_consts = $this->getAbstractConstantNames();
    $ret .= "\n  - Constants [" . (count($consts) + count($abs_consts)) . "] {\n";
    foreach ($consts as $k => $v) {
      $ret .= '    Constant [ ' . gettype($v) . " $k {" . (string)$v . "} ]\n";
    }
    foreach ($abs_consts as $k) {
      $ret .= '    Abstract Constant [ '. $k ."]\n";
    }
    $ret .= "  }\n";

    /* Static Properties */
    $props = $this->getProperties();
    $numStaticProps = 0;
    $numDynamicProps = 0;
    foreach ($props as $prop) {
      if ($prop->isStatic()) {
        ++$numStaticProps;
      } else if (!$prop->isDefault()) {
        ++$numDynamicProps;
      }
    }
    $ret .= "\n  - Static properties [{$numStaticProps}] {\n  ";
    foreach ($props as $prop) {
      if (!$prop->isStatic()) { continue;}
      $ret .= '  ' . str_replace("\n", "\n  ", $prop->__toString());
    }
    $ret .= "}\n";

    /* Static Methods  */
    $funcs = $this->getMethods();
    $numStaticFuncs = 0;
    foreach ($funcs as $func) {
      if ($func->isStatic()) ++$numStaticFuncs;
    }
    $ret .= "\n  - Static methods [{$numStaticFuncs}] {\n";
    foreach ($funcs as $func) {
      if (!$func->isStatic()) continue;
      $ret .= '    ' . str_replace("\n", "\n    ",
                                   rtrim($func->__toString(), "\n")) . "\n";
    }
    $ret .= "  }\n";

    /* Declared Instance Properties */
    $numMemberProps = count($props) - ($numStaticProps + $numDynamicProps);
    $ret .= "\n  - Properties [{$numMemberProps}] {\n  ";
    foreach ($props as $prop) {
      if ($prop->isStatic()) continue;
      if (!$prop->isDefault()) continue;
      $ret .= '  ' . str_replace("\n", "\n  ", $prop->__toString());
    }
    $ret .= "}\n";

    /* Dynamic Instance Properties */
    if ($numDynamicProps) {
      $ret .= "\n  - Dynamic Properties [{$numDynamicProps}] {\n  ";
      foreach ($props as $prop) {
        if ($prop->isStatic()) continue;
        if ($prop->isDefault()) continue;
        $ret .= '  ' . str_replace("\n", "\n  ", $prop->__toString());
      }
      $ret .= "}\n";
    }

    /* Instance Methods */
    $numMemberFuncs = count($funcs) - $numStaticFuncs;
    $ret .= "\n  - Methods [{$numMemberFuncs}] {\n";
    foreach ($funcs as $func) {
      if ($func->isStatic()) continue;
      $ret .= '    ' . str_replace("\n", "\n    ",
                                   rtrim($func->__toString(), "\n")) . "\n";
    }
    $ret .= "  }\n";

    $ret .= "}\n";
    return preg_replace("/(^|\n)\s+(\n|$)/", "\n\n", $ret);
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ReflectionClass'
    );
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.export.php )
   *
   * Exports a reflected class.
   *
   * @name       mixed   The reflection to export.
   * @ret        mixed   Setting to TRUE will return the export, as opposed
   *                     to emitting it. Setting to FALSE (the default) will
   *                     do the opposite.
   *
   * @return     mixed   If the return parameter is set to TRUE, then the
   *                     export is returned as a string, otherwise NULL is
   *                     returned.
   */
  public static function export($name, bool $ret=false): ?string {
    $obj = new ReflectionClass($name);
    $str = (string) $obj;
    if ($ret) {
      return $str;
    }
    print $str;
  }

  <<__Native>>
  private function get()[]: class<mixed>;

  public function getName()[]: string {
    return HH\class_to_classname($this->get());
  }

  <<__Native>>
  private function getParent()[]: ?class<mixed>;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.innamespace.php
   * )
   *
   * Checks if this class is defined in a namespace.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  public function inNamespace()[]: bool {
    return strrpos($this->getName(), '\\') !== false;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.functionabstract.php )
   *
   * Gets the namespace name.
   *
   * @return     string   The namespace name.
   */
  public function getNamespaceName()[]: string {
    $name = $this->getName();
    $pos = strrpos($name, '\\');
    return ($pos === false) ? '' : substr($name, 0, $pos);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getshortname.php )
   *
   * Get the short name of the function (without the namespace part).
   *
   * @return     string  The short name of the function.
   */
  public function getShortName()[]: string {
    $name = $this->getName();
    $pos = strrpos($name, '\\');
    return ($pos === false) ? $name : substr($name, $pos + 1);
  }

  <<__Native>>
  public function isHack()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.isinternal.php )
   *
   * Checks if the class is defined internally by an extension, or the core,
   * as opposed to user-defined.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isInternal()[]: bool;

  public function isUserDefined()[]: bool {
    return !$this->isInternal();
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.isinstantiable.php )
   *
   * Checks if the class is instantiable.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isInstantiable()[]: bool;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.iscloneable.php )
   *
   * Returns whether this class is cloneable.
   *
   * @return     bool   Returns TRUE if the class is cloneable, FALSE otherwise.
   */
  public function isCloneable()[]: bool {
    return $this->isInstantiable() &&
      (!$this->hasMethod('__clone') || $this->getMethod('__clone')->isPublic());
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getmethod.php )
   *
   * Gets a ReflectionMethod for a class method.
   *
   * @name       mixed   The method name to reflect.
   *
   * @return     mixed   A ReflectionMethod.
   */
  public function getMethod($name)[]: ReflectionMethod {
    return new ReflectionMethod($this->getName(), $name);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.hasmethod.php )
   *
   * Checks whether a specific method is defined in a class.
   *
   * @name       mixed   Name of the method being checked for.
   *
   * @return     bool    TRUE if it has the method, otherwise FALSE
   */
  <<__Native>>
  public function hasMethod(string $name)[]: bool;

  /* Helper for getMethods: correctly ordered Set of the methods
   * declared on this class and its parents */
  <<__Native>>
  private static function getMethodOrder(string $clsname, int $filter)[]: keyset;

  private function getMethodOrderWithCaching(?int $filter)[]: keyset<string> {
    if (null === $filter) {
      return self::getMethodOrderCache($this->getName());
    }
    return self::getMethodOrder($this->getName(), $filter);
  }

  <<__Memoize>>
  private static function getMethodOrderCache(
    string $clsname,
  )[]: keyset<string> {
    return self::getMethodOrder($clsname, 0xFFFF);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getmethods.php )
   *
   * Gets an array of methods for the class.
   *
   * @filter     mixed   Filter the results to include only methods with
   *                     certain attributes. Defaults to no filtering.
   *
   *                     Any combination of ReflectionMethod::IS_STATIC,
   *                     ReflectionMethod::IS_PUBLIC,
   *                     ReflectionMethod::IS_PROTECTED,
   *                     ReflectionMethod::IS_PRIVATE,
   *                     ReflectionMethod::IS_ABSTRACT,
   *                     ReflectionMethod::IS_FINAL.
   *
   * @return     mixed   An array of ReflectionMethod objects reflecting each
   *                     method.
   */
  public function getMethods(?int $filter = null)[]: varray<ReflectionMethod> {
    $ret = vec[];
    $clsname = $this->getName();
    foreach ($this->getMethodOrderWithCaching($filter) as $name) {
      $ret[] = new ReflectionMethod($clsname, $name);
    }
    return $ret;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.hasconstant.php
   * )
   *
   * Checks whether the class has a specific constant defined or not.
   *
   * @name       mixed   The name of the constant being checked for.
   *
   * @return     bool   TRUE if the constant is defined, otherwise FALSE.
   */
  <<__Native>>
  public function hasConstant(string $name)[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getconstant.php
   * )
   *
   * Gets the defined constant. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @name       mixed   Name of the constant.
   *
   * @return     mixed   Value of the constant, or FALSE if it doesn't exist
   */
  <<__Native>>
  public function getConstant(string $name)[]: mixed;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getconstants.php
   * )
   *
   * Gets defined constants from a class. Warning: This function is
   * currently not documented; only its argument list is available.
   *
   * @return     array   An array of constants. Constant name in key,
   *                     constant value in value.
   */
  public function getConstants()[]: darray<string, mixed> {
    return self::getConstantsCache($this->getName());
  }

  <<__Memoize>>
  private static function getConstantsCache(
    string $clsname
  )[]: darray<string, mixed> {
    return self::getOrderedConstants($clsname);
  }

  /**
   * ( excerpt from
   *   http://php.net/manual/en/reflectionclass.getabstractconstantnames.php
   * )
   *
   * Returns an array containing the names of abstract constants as both
   * keys and values.
   *
   * @return  array<string, string>
   */
  public function getAbstractConstantNames()[]: darray<string, string> {
    return self::getAbstractConstantNamesCache($this->getName());
  }

  <<__Memoize>>
  private static function getAbstractConstantNamesCache(
    string $clsname
  )[]: darray<string, string> {
    return self::getOrderedAbstractConstants($clsname);
  }

  private function getTypeConstantNamesWithCaching()[]: darray<string, string> {
    return self::getTypeConstantNamesCache($this->getName());
  }

  <<__Memoize>>
  private static function getTypeConstantNamesCache(
    string $clsname
  )[]: darray<string, string> {
    return self::getOrderedTypeConstants($clsname);
  }

  public function getTypeConstant(string $name)[]: ReflectionTypeConstant {
    return new ReflectionTypeConstant($this->getName(), $name);
  }

  public function hasTypeConstant($name)[]: bool {
    return array_key_exists($name, $this->getTypeConstantNamesWithCaching());
  }

  public function getTypeConstants()[]: varray<ReflectionTypeConstant> {
    $ret = vec[];
    $class = $this->getName();
    foreach ($this->getTypeConstantNamesWithCaching() as $name) {
      $ret[] = new ReflectionTypeConstant($class, $name);
    }
    return $ret;
  }

  <<__Native>>
  private static function getOrderedConstants(
    string $clsname
  )[]: darray<string, mixed>;

  <<__Native>>
  private static function getOrderedAbstractConstants(
    string $clsname
  )[]: darray<string, string>;

  <<__Native>>
  private static function getOrderedTypeConstants(
    string $clsname
  )[]: darray<string, string>;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getinterfacenames.php )
   *
   * Get the interface names.
   *
   * @return     mixed   A numerical array with interface names as the
   *                     values.
   */
  <<__Native>>
  public function getInterfaceNames()[]: varray<string>;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getinterfaces.php )
   *
   * Gets the interfaces.
   *
   * @return     mixed   An associative array of interfaces, with keys as
   *                     interface names and the array values as
   *                     ReflectionClass objects.
   */
  public function getInterfaces()[]: darray<string, ReflectionClass> {
    return $this->getReflectionClassesFromNames($this->getInterfaceNames());
  }

  /**
   * Gets the list of implemented interfaces/inherited classes needed to
   * implement an interface / use a trait. Empty array for abstract and
   * concrete classes.
   */
  <<__Native>>
  public function getRequirementNames()[]: varray<string>;

  /**
   * Gets ReflectionClass-es for the requirements of this class
   *
   * @return  An associative array of requirements, with keys as
   *          requirement names and the array values as ReflectionClass objects.
   */
  public function getRequirements()[]: darray<string, ReflectionClass> {
    return $this->getReflectionClassesFromNames($this->getRequirementNames());
  }

  /**
   * Gets the name of the required class for a trait.
   *
   * @return  Null if there are no require class requirement or the
   *          string name of the class required by the require class statement
   */
  <<__Native>>
  public function getRequiredClass()[]: ?string;

  /**
   * Gets the name of the required this as class for a trait.
   *
   * @return  Null if there are no require this as requirement or the
   *          string name of the class required by the require class statement
   */
  <<__Native>>
  public function getRequiredThisAsClass()[]: ?string;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.gettraitnames.php )
   *
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     mixed   Returns an array with trait names in values. Returns
   *                     NULL in case of an error.
   */
  <<__Native>>
  public function getTraitNames()[]: varray<string>;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.gettraits.php )
   *
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     mixed   Returns an array with trait names in keys and
   *                     instances of trait's ReflectionClass in values.
   *                     Returns NULL in case of an error.
   */
  public function getTraits()[]: darray<string, ReflectionClass> {
    return $this->getReflectionClassesFromNames($this->getTraitNames());
  }

  /**
   * Helper for the get{Traits,Interfaces,Requirements} methods
   */
  private function getReflectionClassesFromNames(
    varray<string> $names
  )[]: darray<string, ReflectionClass> {
    $ret = dict[];
    foreach ($names as $name) {
      $ret[$name] = new ReflectionClass($name);
    }
    return $ret;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.isinterface.php
   * )
   *
   * Checks whether the class is an interface.
   *
   * @return     mixed   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isInterface()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.isabstract.php )
   *
   * Checks if the class is abstract.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isAbstract()[]: bool;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.isfinal.php )
   *
   * Checks if a class is final.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isFinal()[]: bool;

  /**
   * Checks if a class is internal.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isInternalToModule()[]: bool;

  /*
   * Returns the module associated with the given function.
   *
   * @return     ?string  Returns the module name if the class is part of a module, null otherwise.
   */
  <<__Native>>
  public function getModule()[]: ?string;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.istrait.php )
   *
   * Returns whether this is a trait.
   *
   * @return     bool   Returns TRUE if this is a trait, FALSE otherwise.
   */
  <<__Native>>
  public function isTrait()[]: bool;

  /**
   * Returns whether this ReflectionClass represents an enum.
   *
   * @return     bool   Returns TRUE if this is an enum, FALSE otherwise.
   */
  <<__Native>>
  public function isEnum()[]: bool;

  /**
   * Returns the underlying type of this ReflectionClass, given that it
   * represents an enum. If it does not, it throws.
   *
   * @return     string   the string representation of the underlying type.
   */
  <<__Native>>
  public function getEnumUnderlyingType()[]: ?string;


  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getmodifiers.php
   * )
   *
   * Returns a bitfield of the access modifiers for this class.
   *
   * @return     int   Returns bitmask of modifier constants.
   */
  <<__Native>>
  public function getModifiers()[]: int;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.isinstance.php )
   *
   * Checks if an object is an instance of a class.
   *
   * @obj        mixed   The object being compared to.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  public function isInstance($obj)[]: bool {
    return is_a($obj, $this->getName());
  }

  <<__Native>>
  private function getConstructorName()[]: ?string;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getconstructor.php )
   *
   * Gets the constructor of the reflected class.
   *
   * @return     mixed   A ReflectionMethod object reflecting the class'
   *                     constructor, or NULL if the class has no
   *                     constructor.
   */
  public function getConstructor()[]: ?ReflectionMethod {
    $constructor_name = $this->getConstructorName();
    return $constructor_name ? $this->getMethod($constructor_name): null;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.newinstance.php
   * )
   *
   * Creates a new instance of the class. The given arguments are passed to
   * the class constructor.
   */
  public function newInstance(...$args) {
    if ($args && !$this->getConstructorName()) {
      // consistent with reference, but perhaps not particularly useful
      throw new ReflectionException(
        'Class '.$this->getName().' lacks a constructor, so you cannot pass'
        .' any constructor arguments'
      );
    }
    return hphp_create_object($this->getName(), $args);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.newinstanceargs.php )
   *
   * Creates a new instance of the class, the given arguments are passed to
   * the class constructor.
   *
   * @args       mixed   The parameters to be passed to the class constructor
   *                     as an array.
   *
   * @return     mixed   Returns a new instance of the class.
   */
  public function newInstanceArgs(Traversable<mixed> $args = vec[])[defaults] {
    if ($args && !$this->getConstructorName()) {
      // consistent with reference, but perhaps not particularly useful
      throw new ReflectionException(
        'Class '.$this->getName().' lacks a constructor, so you cannot pass'
        .' any constructor arguments'
      );
    }
    return hphp_create_object($this->getName(), varray($args));
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.newinstancewithoutconstructor.php
   * )
   *
   * Creates a new instance of the class without invoking the constructor.
   */
  public function newInstanceWithoutConstructor()[] {
    return hphp_create_object_without_constructor($this->getName());
  }

  // This calculations requires walking the preclasses in the hierarchy and
  // should not be getting performed repeatedly.
  <<__Native>>
  // returns keyset of instance and then static properties
  private static function getNonDynamicPropNames(string $clsname)[]: keyset<string>;

  <<__Native>>
  private function getDynamicPropertyInfos(
    \HH\object $obj,
  )[]: dict<string, mixed>;

  private function getOrderedPropertyNames()[]: keyset<string> {
    $prop_names = self::getNonDynamicPropNamesMemoized($this->getName());
    if (!$this->obj) { return $prop_names; }

    // caching cannot be well applied to an object's dynamic properties,
    // since they can be added and removed at any time between calls to
    // property methods.
    $dynamic_props = $this->getDynamicPropertyInfos($this->obj);
    return HH\Lib\Keyset\union($prop_names, HH\Lib\Keyset\keys($dynamic_props));
  }

  <<__Memoize>>
  private static function getNonDynamicPropNamesMemoized(
    string $clsname
  )[]: keyset<string> {
    return self::getNonDynamicPropNames($clsname);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getproperty.php
   * )
   *
   * Gets a ReflectionProperty for a class's property.
   *
   * @name       mixed   The property name.
   *
   * @return     mixed   A ReflectionProperty.
   */
  public function getProperty($name)[] {
    $class = $this->name;
    if (!$this->hasProperty($name)) {
      throw new ReflectionException("Property $class::$name does not exist");
    }
    if ($this->obj) {
      return new ReflectionProperty($this->obj, $name);
    } else {
      return new ReflectionProperty($this->name, $name);
    }
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.hasproperty.php
   * )
   *
   * Checks whether the specified property is defined.
   *
   * @name       mixed  Name of the property being checked for.
   *
   * @return     bool   TRUE if it has the property, otherwise FALSE
   */
  public function hasProperty($name)[]: bool {
    return HH\Lib\C\contains($this->getOrderedPropertyNames(), $name);
  }

  /**
   * ( excerpt* http://php.net/manual/en/reflectionclass.getproperties.php )
   *
   * Retrieves reflected properties.
   *
   * @filter     mixed   The optional filter, for filtering desired property
   *                     types. It's configured using the ReflectionProperty
   *                     constants, and defaults to all property types.
   *
   * @return     mixed   An array of ReflectionProperty objects.
   */
  public function getProperties($filter = 0xFFFF)[]: varray<ReflectionProperty> {
    $ret = vec[];
    foreach ($this->getOrderedPropertyNames() as $name) {
      if ($this->obj) {
        $p = new ReflectionProperty($this->obj, $name);
      } else {
        $p = new ReflectionProperty($this->name, $name);
      }
      if (($filter & ReflectionProperty::IS_PUBLIC)    && $p->isPublic()    ||
          ($filter & ReflectionProperty::IS_PROTECTED) && $p->isProtected() ||
          ($filter & ReflectionProperty::IS_PRIVATE)   && $p->isPrivate()   ||
          ($filter & ReflectionProperty::IS_STATIC)    && $p->isStatic()) {
        $ret[] = $p;
      }
    }
    return $ret;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getstaticproperties.php )
   *
   * Get the static properties. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     mixed   The static properties, as an array.
   */
  public function getStaticProperties(): darray<string, mixed> {
    $ret = dict[];
    foreach ($this->getProperties(ReflectionProperty::IS_STATIC) as $prop) {
      $val = hphp_get_static_property($this->getName(), $prop->name, true);
      $ret[$prop->name] = $val;
    }
    return $ret;
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getstaticpropertyvalue.php )
   *
   * Gets the value of a static property on this class.
   *
   * @name       mixed   The name of the static property for which to return
   *                     a value.
   * @default    mixed
   *
   * @return     mixed   The value of the static property.
   */
  public function getStaticPropertyValue($name, ...$args) {
    // We can't check if a parameter isn't passed,
    // we can only check its default value, but that fails
    // if I want to pass the default value.
    // Use variadic args for this.
    if ($this->hasProperty($name) &&
        $this->getProperty($name)->isStatic()) {
      return hphp_get_static_property($this->getName(), $name, false);
    } else if (!array_key_exists(0, $args)) {
      throw new ReflectionException(
        sprintf("Class %s does not have a property named %s",
                $this->getName(), $name)
      );
    }
    return $args[0];
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.setstaticpropertyvalue.php )
   *
   * Sets static property value. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @name       mixed   Property name.
   * @value      mixed   New property value.
   */
  public function setStaticPropertyValue($name, $value): void {
    // XXX: should be __Native, not a builtin
    hphp_set_static_property($this->getName(), $name, $value, false);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getdefaultproperties.php )
   *
   * Gets default properties from a class (including inherited properties).
   *
   * This method only works for static properties when the default value is
   * known statically. If it is not known statically you will get null instead
   * of the correct value. Do not rely on this API for default values of
   * static properties.
   *
   * @return     mixed   An array of default properties, with the key being
   *                     the name of the property and the value being the
   *                     default value of the property or NULL if the
   *                     property doesn't have a default value. The function
   *                     does not distinguish between static and non static
   *                     properties and does not take visibility modifiers
   *                     into account.
   */
  public function getDefaultProperties()[]: darray<string, mixed> {
    $ret = dict[];
    foreach ($this->getProperties() as $prop) {
      if ($prop->isDefault()) {
        $ret[$prop->name] = $prop->getDefaultValue();
      }
    }
    return $ret;
  }

  /**
   * Get names of type vars defined on this class.
   *
   * @return vec containing type var names as strings
   */
  <<__Native>>
  public function getTypeVarNames()[]: vec<string>;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getextension.php
   * )
   *
   * Gets a ReflectionExtension object for the extension which defined the
   * class.
   *
   * @return     mixed   A ReflectionExtension object representing the
   *                     extension which defined the class, or NULL for
   *                     user-defined classes.
   */
  public function getExtension()[]: ?ReflectionExtension {
    // FIXME: HHVM doesn't support extension info
    return new ReflectionExtension($this->getExtensionName());
    // Truthful implementation would be:
    // return null;
    // A serious implementation would be:
    // return new ReflectionExtension($this->getExtensionName());
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getextensionname.php )
   *
   * Gets the name of the extension which defined the class.
   *
   * @return     mixed   The name of the extension which defined the class,
   *                     or FALSE for user-defined classes.
   */
  public function getExtensionName()[]: mixed {
    // FIXME: HHVM doesn't support extension info
    return '';
    // Truthful implementation would be:
    // return false;
    // A serious implementation would have this function __Native
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.isiterateable.php )
   *
   * Checks whether the class is iterateable.
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  public function isIterateable()[]: bool {
    return $this->isSubclassOf(\HH\Traversable::class);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.implementsinterface.php )
   *
   * Checks whether it implements an interface.
   *
   * @cls        mixed   The interface name.
   *
   * @return     bool    Returns TRUE on success or FALSE on failure.
   */
  public function implementsInterface($cls)[]: bool {
    if ($cls is ReflectionClass) { $cls = $cls->getName(); }

    // Normalize to avoid autoloading twice for undefined classes
    $normalized_cls = $cls[0] == '\\' ? substr($cls, 1) : $cls;
    if (!interface_exists($normalized_cls)) {
      throw new ReflectionException("Interface $normalized_cls does not exist");
    }
    return $this->isInterface()
      ? is_a($this->getName(), $normalized_cls, true)
      : $this->isSubclassOf($normalized_cls);
  }

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getparentclass.php )
   *
   * Warning: This function is currently not documented; only its argument
   * list is available.
   *
   * @return     mixed   A ReflectionClass, or false.
   */
  public function getParentClass()[]: mixed {
    $parent = $this->getParent();
    return $parent !== null ? new ReflectionClass($parent) : false;
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.issubclassof.php
   * )
   *
   * Checks if the class is a subclass of a specified class or implements a
   * specified interface.
   *
   * @cls        mixed   The class name being checked against.
   *
   * @return     bool    Returns TRUE on success or FALSE on failure.
   */
  public function isSubclassOf($cls)[]: bool {
    if ($cls is ReflectionClass) {
      $cls = $cls->getName();
    }
    return is_subclass_of($this->getName(), $cls);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getfilename.php
   * )
   *
   * Gets the filename of the file in which the class has been defined.
   *
   * @return     mixed   Returns the filename of the file in which the class
   *                     has been defined. If the class is defined in the PHP
   *                     core or in a PHP extension, FALSE is returned.
   */
  <<__Native>>
  public function getFileName()[]: mixed;

  /**
   * Gets the declaring file for the reflected class.
   *
   * @return ReflectionFile   A ReflectionFile object of the file that the
   *                           reflected class is part of.
   */
  public function getFile()[]: ReflectionFile {
    $fileName = $this->getFileName();

    if ($fileName === false) {
      throw new ReflectionException(
        'Couldn\'t get ReflectionFile because the class was not defined in a '.
        'file.'
      );
    }

    if (!is_string($fileName)) {
      throw new ReflectionException(
        'Unexpected non-string file name for ReflectionClass.'
      );
    }

    return new ReflectionFile((string)$fileName);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getstartline.php
   * )
   *
   * Get the starting line number. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     int   The starting line number, as an integer.
   */
  <<__Native>>
  public function getStartLine()[]: mixed;

  /**
   * ( excerpt from http://php.net/manual/en/reflectionclass.getendline.php )
   *
   * Gets end line number from a user-defined class definition.
   *
   * @return     int   The ending line number of the user defined class, or
   *                   FALSE if unknown.
   */
  <<__Native>>
  public function getEndLine()[]: mixed;

  /**
   * ( excerpt from
   * http://php.net/manual/en/reflectionclass.getdoccomment.php )
   *
   * Gets doc comments from a class. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @return     mixed   The doc comment if it exists, otherwise FALSE
   */
  <<__Native>>
  public function getDocComment()[]: mixed;

  use ReflectionTypedAttribute;

  <<__Native>>
  public function getAttributesNamespaced()[]: darray<string, varray<mixed>>;

  use ReflectionLegacyAttribute;

  <<__Native>>
  public function getAttributesRecursiveNamespaced(
  ): darray<string, varray<mixed>>;

  <<__Native>>
  public function getReifiedTypeParamInfo()[]: varray;

}

///////////////////////////////////////////////////////////////////////////////
// object

/**
 * ( excerpt from http://php.net/manual/en/class.reflectionobject.php )
 *
 * The ReflectionObject class reports information about an object.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
class ReflectionObject extends ReflectionClass {

  /**
   * ( excerpt from http://php.net/manual/en/reflectionobject.construct.php
   * )
   *
   *  Constructs a ReflectionObject.
   */
  public function __construct($argument)[] {
    if (!is_object($argument)) {
      throw new ReflectionException(
        __CLASS__.' expects to be constructed with an object, got '
        .gettype($argument)
      );
    }
    parent::__construct($argument);
  }

  /**
   * ( excerpt from http://php.net/manual/en/reflectionobject.export.php )
   *
   * Exports a reflection. Warning: This function is currently not
   * documented; only its argument list is available.
   *
   * @obj        mixed   The reflection to export.
   * @ret        mixed   Setting to TRUE will return the export, as opposed
   *                     to emitting it. Setting to FALSE (the default) will
   *                     do the opposite.
   *
   * @return     mixed   If the return parameter is set to TRUE, then the
   *                     export is returned as a string, otherwise NULL is
   *                     returned.
   */
  public static function export($obj, $ret=false) {
    $obj = new ReflectionObject($obj);
    $str = (string)$obj;
    if ($ret) {
      return $str;
    }
    print $str;
  }
}

///////////////////////////////////////////////////////////////////////////////
// type constant

/**
 * The ReflectionTypeConstant class reports information about an object.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
<<__NativeData>>
class ReflectionTypeConstant implements Reflector {

  /**
   * Constructs a new ReflectionTypeConstant.
   *
   * @cls        mixed   Classname or object (instance of the class) that
   *                     contains the type constant.
   * @name       string  Name of the type constant.
   */
  public function __construct(mixed $cls, string $name)[] {
    if (!$this->__init($cls, (string) $name)) {
      $classname = is_object($cls) ? get_class($cls) : $cls;
      throw new ReflectionException(
        "Type Constant $classname::$name does not exist");
    }
  }

  /**
   * Get the name of the type constant.
   *
   * @return     string   The name of the type constant.
   */
  <<__Native>>
  public function getName()[]: ?string;

  /**
   * Checks if the type constant is abstract
   *
   * @return     bool   Returns TRUE on success or FALSE on failure.
   */
  <<__Native>>
  public function isAbstract()[]: bool;

  /**
   * Get the type assigned to this type constant as a string
   *
   * @return     NULL | string   The assigned type or null if is abstract
   */
  public function getAssignedTypeText()[]: ?string {
    return $this->getAssignedTypeHint() ?: null;
  }

  /**
   * Gets the declaring class for the reflected type constant. This is
   * the most derived class in which the type constant is declared.
   *
   * @return ReflectionClass   A ReflectionClass object of the class that the
   *                           reflected type constant is part of.
   */
  public function getDeclaringClass()[] {
    return new ReflectionClass($this->getDeclaringClassname());
  }

  /**
   * Gets the class for the reflected type constant.
   *
   * @return ReflectionClass   A ReflectionClass object of the class that the
   *                           reflected type constant is part of.
   */
  public function getClass()[] {
    return new ReflectionClass($this->getClassPtr());
  }

    /**
   * Gets the name of the "canonical" class, where canonical means that
   * the type constant is *actually* declared and defined on that class.
   * This is particularly relevant when the const is inherited from a trait
   *
   * NOTE: This wouldn't work "correctly" in repo-mode, where traits are
   * flattened. As such, in repo-mode this throws an exception instead.
   */
  <<__Native>>
  public function getCanonicalClassname()[]: ?string;

  public function __toString()[] {
    $abstract = $this->isAbstract() ? 'abstract ' : '';

    $type_text = $this->getAssignedTypeText() ?? '';
    $val = $this->isAbstract() ? '' : " = $type_text";

    return "TypeConstant [ {$abstract}const type {$this->getName()}{$val}]\n";
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ReflectionTypeConstant'
    );
  }

  public static function export($cls, $name, $ret=false) {
    $obj = new self($cls, $name);
    $str = (string)$obj;
    if ($ret) {
      return $str;
    }
    print $str;
  }

  <<__Native>>
  private function __init(mixed $cls_or_obj, string $const)[]: bool;

  <<__Native>>
  private function getAssignedTypeHint()[]: ?string;

  <<__Native>>
  private function getDeclaringClassname()[]: ?string;

  <<__Native>>
  private function getClassPtr()[]: class<mixed>;

  /* returns the shape containing the full type information for this
   * type constant. The structure of this shape is specified in
   * reflection.hhi. */
  public function getTypeStructure()[] {
    return HH\type_structure(
      $this->getClassPtr(),
      $this->getName()
    );
  }

}

///////////////////////////////////////////////////////////////////////////////
// type aliases

/**
 * The ReflectionTypeAlias class reports information about a type
 * alias.
 *
 * The purpose of reflection is to enable tooling for introspection of the
 * codebase, not to drive execution of the business logic.
 */
<<__NativeData>>
class ReflectionTypeAlias implements Reflector {

  private string $name = '';

  /**
   * Constructs a new ReflectionTypeAlias.
   *
   * @name      string  Name of the type alias.
   */
  final public function __construct(string $name)[] {
    $n = $this->__init($name);
    if (!$n) {
      throw new ReflectionException(
        "type alias {$name} does not exist");
    }
    $this->name = $n;
  }

  // helper for ctor
  <<__Native>>
  private function __init(string $name)[write_this_props]: ?string;

  /**
   * Get the TypeStructure that contains the full type information of
   * the assigned type.
   *
   * @return    array  The type structure of the type alias.
   */
  <<__Native>>
  public function getTypeStructure()[]: darray;

  /**
   * Gets all attributes
   *
   * @return  darray<arraykey, varray<mixed>>
   */
  <<__Native>>
  final public function getAttributesNamespaced(
  )[]: darray<arraykey, varray<mixed>>;

  use ReflectionLegacyAttribute;

  use ReflectionTypedAttribute;

  /**
   * Get the TypeStructure with type information resolved. Call at
   * your own peril as non-hoisted classes might cause fatal.
   *
   * @return    array  The resolved type structure of the type alias.
   */
  public function getResolvedTypeStructure()[] {
    return HH\type_structure($this->name);
  }

  /**
   * Get the assigned type as a string.
   *
   * @return    string The assigned type.
   */
  <<__Native>>
  public function getAssignedTypeText()[]: ?string;

  /**
   * Get the name of the type alias.
   *
   * @return    string  The name of the type alias
   */
  public function getName()[] {
    return $this->name;
  }

  /**
   * Get the name of the file in which the type alias was defined.
   */
  <<__Native>>
  public function getFileName()[]: ?string;

  /**
   * Gets the declaring file for the reflected type alias.
   *
   * @return ReflectionFile   A ReflectionFile object of the file that the
   *                           reflected type alias is part of.
   */
  public function getFile()[] {
    return new ReflectionFile($this->getFileName());
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ReflectionTypeAlias'
    );
  }

  public function __toString()[] {
    return "TypeAlias [ {$this->name} : {$this->getAssignedTypeText()} ]\n";
  }

}


///////////////////////////////////////////////////////////////////////////////
// files

/** The ReflectionFile class reports information about a file.
 */
<<__NativeData>>
final class ReflectionFile implements Reflector {

  private string $name = '';

  /**
   * Constructs a new ReflectionFile.
   *
   * @name      string  Name of the file.
   */
  final public function __construct(string $name)[] {
    $n = $this->__init($name);
    if (!$n) {
      throw new ReflectionException(
        "file {$name} does not exist");
    }
    $this->name = $n;
  }

  // helper for ctor
  <<__Native>>
  private function __init(string $name)[]: ?string;

  /**
   * Gets all attributes
   *
   * @return  darray<arraykey, varray<mixed>>
   */
  <<__Native>>
  final public function getAttributesNamespaced(
  )[]: darray<arraykey, varray<mixed>>;

  use ReflectionLegacyAttribute;

  use ReflectionTypedAttribute;

  /**
   * Get the name of the file.
   *
   * @return    string  The name of the file
   */
  public function getName()[] {
    return $this->name;
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ReflectionFile'
    );
  }

  public function __toString()[] {
    return "File [ {$this->name} ]\n";
  }

  /*
   * Get the module of the file.
   *
   * @return     ?string  Returns the module name if the file has a module
   *                      membership statement, null otherwise.
   */
  <<__Native>>
  public function getModule()[]: ?string;

}

///////////////////////////////////////////////////////////////////////////////
// modules

/** The ReflectionModule class reports information about a module.
 */
<<__NativeData>>
final class ReflectionModule implements Reflector {

  private string $name = '';

  /**
   * Constructs a new ReflectionModule.
   *
   * @name      string  Name of the module.
   */
  final public function __construct(string $name)[] {
    $this->__init($name); // throws an exception if module doesn't exist
    $this->name = $name;
  }

  // helper for ctor
  <<__Native>>
  private function __init(string $name)[]: void;

  /**
   * Gets all attributes
   *
   * @return  darray<arraykey, varray<mixed>>
   */
  <<__Native>>
  final public function getAttributesNamespaced(
  )[]: darray<arraykey, varray<mixed>>;

  use ReflectionLegacyAttribute;

  use ReflectionTypedAttribute;

  <<__Native>>
  public function getDocComment()[]: mixed;

  /**
   * Get the name of the file.
   *
   * @return    string  The name of the file
   */
  public function getName()[] {
    return $this->name;
  }

  // Prevent cloning
  final public function __clone() {
    throw new BadMethodCallException(
      'Trying to clone an uncloneable object of class ReflectionModule'
    );
  }

  public function __toString()[] {
    return "Module [ {$this->name} ]\n";
  }

}
