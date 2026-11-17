<?hh /* -*- mode: php -*- */
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 */
<<file:__EnableUnstableFeatures('class_type')>>
interface Reflector extends IPureStringishObject {
  public function __toString()[]: string;
}

class Reflection {
  public static function getModifierNames(
    int $modifiers,
  )[]: vec<string>;
  public static function export(
    Reflector $reflector,
    bool $return = false,
  ): HH\FIXME\MISSING_RETURN_TYPE;
}

class ReflectionClass implements Reflector {

  const int IS_IMPLICIT_ABSTRACT;
  const int IS_EXPLICIT_ABSTRACT;
  const int IS_FINAL;

  /**
   * This field is read-only
   */
  public string $name;

  public function __construct(class<mixed> $argument)[];
  public static function export(mixed $argument, bool $return = false): ?string;
  public function getConstant(string $name)[]: mixed;
  public function getConstants()[]: darray<string, mixed>;
  public function getAbstractConstantNames()[]: darray<string, string>;
  public function getTypeConstant(string $name)[]: ReflectionTypeConstant;
  public function getTypeConstants()[]: varray<ReflectionTypeConstant>;
  public function getConstructor()[]: ?ReflectionMethod;
  public function getDefaultProperties()[]: darray<string, mixed>;
  public function getTypeVarNames()[]: vec<string>;
  /**
   * Returns string or false
   */
  public function getDocComment()[]: mixed;
  public function getEndLine()[]: int;
  public function getExtension()[]: ?ReflectionExtension;
  public function getExtensionName()[]: string;
  /**
   * Returns string or false
   */
  public function getFileName()[]: mixed;
  public function getFile()[]: ReflectionFile;
  public function getInterfaceNames()[]: varray<string>;
  public function getInterfaces()[]: darray<string, ReflectionClass>;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  final public function getAttributeClass<T as HH\ClassLikeAttribute>(
    class<T> $c,
  )[]: ?T;
  public function getMethod(string $name)[]: ReflectionMethod;
  public function getMethods(?int $filter = null)[]: varray<ReflectionMethod>;
  public function getModifiers()[]: int;
  public function getName()[]: classname<mixed>;
  public function getNamespaceName()[]: string;
  public function isInternalToModule()[]: bool;
  public function getModule()[]: ?string;
  /**
   * Returns ReflectionClass or false
   */
  public function getParentClass()[]: mixed;
  public function getProperties(
    int $filter = 0xFFFF,
  )[]: varray<ReflectionProperty>;
  public function getProperty(string $name)[]: ReflectionProperty;
  public function getRequirementNames()[]: varray<string>;
  public function getRequirements()[]: darray<string, ReflectionClass>;
  public function getRequiredClass()[]: ?string;
  public function getRequiredThisAsClass()[]: ?string;
  public function getShortName()[]: string;
  public function getStartLine()[]: int;
  public function getStaticProperties(): darray<string, mixed>;
  public function getStaticPropertyValue(
    string $name,
    mixed $def_value = null,
  ): mixed;
  public function getTraitNames()[]: varray<string>;
  public function getTraits()[]: darray<string, ReflectionClass>;
  public function hasConstant(string $name)[]: bool;
  public function hasMethod(string $name)[]: bool;
  public function hasProperty(string $name)[]: bool;
  public function hasTypeConstant(string $name)[]: bool;
  public function implementsInterface(string $interface)[]: bool;
  public function inNamespace()[]: bool;
  public function isAbstract()[]: bool;
  public function isCloneable()[]: bool;
  public function isFinal()[]: bool;
  public function isInstance(mixed $object)[]: bool;
  public function isInstantiable()[]: bool;
  public function isInterface()[]: bool;
  public function isEnum()[]: bool;
  public function getEnumUnderlyingType()[]: string;
  public function isInternal()[]: bool;
  public function isIterateable()[]: bool;
  /**
   * $class is string or ReflectionClass
   */
  public function isSubclassOf(mixed $class)[]: bool;
  public function isTrait()[]: bool;
  public function isUserDefined()[]: bool;
  <<__Deprecated("Use HH_FIXME::reflectionClassNewInstanceForCallsite instead")>>
  public function newInstance(
    HH\FIXME\MISSING_PARAM_TYPE ...$args
  ): HH\FIXME\MISSING_RETURN_TYPE;
  <<__Deprecated("Use HH_FIXME::reflectionClassNewInstanceArgsForCallsite instead")>>
  public function newInstanceArgs(
    Traversable<mixed> $args = vec[],
  )[defaults]: HH\FIXME\MISSING_RETURN_TYPE;
  public function newInstanceWithoutConstructor(
  )[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function setStaticPropertyValue(string $name, mixed $value): void;
  public function __toString()[]: string;
  public function getReifiedTypeParamInfo()[]: varray<shape(
    'is_reified' => bool,
    'is_soft' => bool,
    'is_warn' => bool,
  )>;
}

class ReflectionObject extends ReflectionClass {
  // Closest sound narrowing available: Hack has no object typehint, so
  // strings still typecheck here and are rejected at runtime.
  public function __construct(nonnull $argument)[];
}

class ReflectionException extends Exception {
  use ExceptionWithPureGetMessageTrait;
}

abstract class ReflectionFunctionAbstract implements Reflector {

  public HH\FIXME\MISSING_PROP_TYPE $name = '';

  // Methods
  public function getName()[]: string;
  public function inNamespace()[]: bool;
  public function getNamespaceName()[]: string;
  public function getShortName()[]: string;
  public function getParamTypeVarNames()[]: keyset<string>;
  public function getTypeVarNames()[]: vec<string>;
  public function isHack()[]: bool;
  public function isInternal()[]: bool;
  public function isClosure()[]: bool;
  public function isGenerator()[]: bool;
  public function returnsReadonly()[]: bool;
  public function isAsync()[]: bool;
  public function isVariadic()[]: bool;
  public function isUserDefined()[]: bool;
  public function getFileName()[]: mixed; // string | false
  public function getFile()[]: ReflectionFile;
  public function getStartLine()[]: mixed; // int | false
  public function getEndLine()[]: mixed; // int | false
  public function getDocComment()[]: mixed; // string | false
  public function getReturnTypeText()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getModule()[]: ?string;
  public function isInternalToModule()[]: bool;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  public function getNumberOfParameters()[]: int;
  public function getParameters()[]: varray<ReflectionParameter>;
  public function getNumberOfRequiredParameters(
  )[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isDeprecated()[]: bool;
  public function getExtension()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getExtensionName()[]: HH\FIXME\MISSING_RETURN_TYPE;
  private function __clone(): void;
  public function hasReturnType()[]: bool;
  public function getReturnType()[]: ?ReflectionType;
  public function getReifiedTypeParamInfo()[]: varray<darray<string, bool>>;
  public function getCoeffects()[]: vec<string>;
}

class ReflectionFunction
  extends ReflectionFunctionAbstract
  implements Reflector {

  public HH\FIXME\MISSING_PROP_TYPE $name = '';

  public function __construct(HH\FIXME\MISSING_PARAM_TYPE $name)[];
  public function __toString()[]: string;
  public static function export(
    HH\FIXME\MISSING_PARAM_TYPE $name,
    HH\FIXME\MISSING_PARAM_TYPE $return = null,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function isDisabled()[]: bool;
  public function invoke(mixed ...$args): HH\FIXME\MISSING_RETURN_TYPE;
  public function invokeArgs(vec<mixed> $args): HH\FIXME\MISSING_RETURN_TYPE;
  public function getClosure(): HH\FIXME\MISSING_RETURN_TYPE;
  final public function getAttributeClass<T as HH\FunctionAttribute>(
    class<T> $c,
  )[]: ?T;
}

class ReflectionMethod extends ReflectionFunctionAbstract implements Reflector {
  // Constants
  const int IS_STATIC;
  const int IS_PUBLIC;
  const int IS_PROTECTED;
  const int IS_PRIVATE;
  const int IS_ABSTRACT;
  const int IS_FINAL;

  public HH\FIXME\MISSING_PROP_TYPE $name = '';
  public HH\FIXME\MISSING_PROP_TYPE $class = '';

  public static function export(
    string $class,
    string $name,
    bool $return = false,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function __construct(
    HH\FIXME\MISSING_PARAM_TYPE $class,
    HH\FIXME\MISSING_PARAM_TYPE $name = null,
  )[];

  public function __toString()[]: string;
  public function isPublic()[]: bool;
  public function isPrivate()[]: bool;
  public function isProtected()[]: bool;
  public function isAbstract()[]: bool;
  public function isFinal()[]: bool;
  public function isStatic()[]: bool;
  public function isReadonly()[]: bool;
  public function isConstructor()[]: bool;
  <<__Deprecated("Use HH_FIXME::reflectionMethodGetClosureForCallsite instead")>>
  public function getClosure(
    mixed $object = null,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function getModifiers()[]: int;
  <<__Deprecated("Use HH_FIXME::reflectionMethodInvokeForCallsite instead")>>
  public function invoke(
    mixed $object,
    mixed ...$args
  ): HH\FIXME\MISSING_RETURN_TYPE;
  <<__Deprecated("Use HH_FIXME::reflectionMethodInvokeArgsForCallsite instead")>>
  public function invokeArgs(
    mixed $object,
    vec<mixed> $args,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function getDeclaringClass()[]: ReflectionClass;
  public function getOriginalClassname()[]: string;
  public function getPrototype()[]: ReflectionMethod;
  <<__PHPStdLib>>
  public function setAccessible(bool $accessible)[write_props]: void;
  final public function getAttributeClass<T as HH\MethodAttribute>(
    class<T> $c,
  )[]: ?T;

  /**
   * Gets the "canonical" version of the method, where canonical means that
   * the method is *actually* defined and implemented on the class that is
   * within the returned value's ->getDeclaringClass()->getName().
   * This is particularly relevant when the method is inherited from a trait
   *
   * NOTE: This wouldn't work "correctly" in repo-mode, where traits are
   * flattened. As such, in repo-mode this throws an exception instead.
   */
  public function getCanonicalMethod()[]: ReflectionMethod;
  public function getCanonicalClassname()[]: string;
}

class ReflectionParameter implements Reflector {
  public HH\FIXME\MISSING_PROP_TYPE $name = '';

  private function __clone(): void;
  public static function export(
    HH\FIXME\MISSING_PARAM_TYPE $function,
    HH\FIXME\MISSING_PARAM_TYPE $parameter,
    HH\FIXME\MISSING_PARAM_TYPE $return = null,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function __construct(
    HH\FIXME\MISSING_PARAM_TYPE $function,
    HH\FIXME\MISSING_PARAM_TYPE $parameter,
    HH\FIXME\MISSING_PARAM_TYPE $info = null,
  )[];
  public function __toString()[]: string;
  public function getName()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isPassedByReference()[]: bool;
  public function isInOut()[]: bool;
  public function isReadonly()[]: bool;
  public function isNamed()[]: bool;
  public function canBePassedByValue()[]: bool;
  public function getDeclaringFunction()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getDeclaringClass()[]: ?ReflectionClass;


  public function isCallable()[]: bool;
  public function allowsNull()[]: bool;
  public function getPosition()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isOptional()[]: bool;
  public function isDefaultValueAvailable()[]: bool;
  public function getDefaultValue()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isDefaultValueConstant()[]: ?bool;
  public function getDefaultValueConstantName()[]: HH\FIXME\MISSING_RETURN_TYPE;
  /**
   * This is the "user type" of the parameter, which is meant to represent the
   * literal type the user has written in the source. <<__Soft>> is rendered
   * as "@" by this function.
   */
  public function getTypeText()[]: string;
  public function getDefaultValueText()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isVariadic()[]: bool;
  public function hasType()[]: bool;
  public function getType()[]: ?ReflectionType;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  final public function getAttributeClass<T as HH\ParameterAttribute>(
    class<T> $c,
  )[]: ?T;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
}

class ReflectionProperty implements Reflector {
  const int IS_STATIC;
  const int IS_PUBLIC;
  const int IS_PROTECTED;
  const int IS_PRIVATE;

  public HH\FIXME\MISSING_PROP_TYPE $name = '';
  public HH\FIXME\MISSING_PROP_TYPE $class = '';

  private function __clone(): void;
  public static function export(
    HH\FIXME\MISSING_PARAM_TYPE $class,
    HH\FIXME\MISSING_PARAM_TYPE $name,
    HH\FIXME\MISSING_PARAM_TYPE $return = null,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function __construct(
    HH\FIXME\MISSING_PARAM_TYPE $class,
    string $name,
  )[];
  public function __toString()[]: string;
  public function getName()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getValue(
    HH\FIXME\MISSING_PARAM_TYPE $obj = null,
  )[read_globals]: HH\FIXME\MISSING_RETURN_TYPE;
  public function setValue(
    HH\FIXME\MISSING_PARAM_TYPE $obj,
    HH\FIXME\MISSING_PARAM_TYPE $value = null,
  )[globals, write_props]: HH\FIXME\MISSING_RETURN_TYPE;
  public function isPublic()[]: bool;
  public function isPrivate()[]: bool;
  public function isProtected()[]: bool;
  public function isStatic()[]: bool;
  public function isReadonly()[]: bool;
  public function isDefault()[]: bool;
  public function getModifiers()[]: int;
  public function getDeclaringClass()[]: ReflectionClass;
  public function getDocComment()[]: HH\FIXME\MISSING_RETURN_TYPE;
  <<__PHPStdLib>>
  public function setAccessible(bool $accessible)[write_props]: void;
  public function getTypeText()[]: string;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
}

class ReflectionExtension implements Reflector {

  public HH\FIXME\MISSING_PROP_TYPE $name = '';

  private function __clone(): void;
  public static function export(
    HH\FIXME\MISSING_PARAM_TYPE $name,
    HH\FIXME\MISSING_PARAM_TYPE $return = false,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function __construct(HH\FIXME\MISSING_PARAM_TYPE $name)[];
  public function __toString()[]: string;
  public function getName()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getVersion()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getFunctions()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getConstants()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getINIEntries()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getClasses()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function getClassNames()[]: HH\FIXME\MISSING_RETURN_TYPE;
  public function info()[]: HH\FIXME\MISSING_RETURN_TYPE;
}

class ReflectionTypeConstant implements Reflector {

  private function __clone(): void;
  public static function export(
    HH\FIXME\MISSING_PARAM_TYPE $class,
    string $name,
    HH\FIXME\MISSING_PARAM_TYPE $return = null,
  ): HH\FIXME\MISSING_RETURN_TYPE;
  public function __construct(
    HH\FIXME\MISSING_PARAM_TYPE $class,
    string $name,
  )[];
  public function __toString()[]: string;
  public function getName()[]: string;
  public function isAbstract()[]: bool;
  public function getDeclaringClass()[]: ReflectionClass;
  public function getClass()[]: ReflectionClass;
  public function getAssignedTypeText()[]: ?string;
  public function getTypeStructure()[]: darray<arraykey, mixed>;
    /**
   * Gets the name of the "canonical" class, where canonical means that
   * the type constant is *actually* declared and defined on that class.
   * This is particularly relevant when the const is inherited from a trait
   *
   * NOTE: This wouldn't work "correctly" in repo-mode, where traits are
   * flattened. As such, in repo-mode this throws an exception instead.
   */
  public function getCanonicalClassname()[]: string;
}

class ReflectionTypeAlias implements Reflector {
  private function __clone(): void;
  final public function __construct(string $name)[];
  public function __toString()[]: string;
  public function getTypeStructure()[]: darray<arraykey, mixed>;
  public function getResolvedTypeStructure()[]: darray<arraykey, mixed>;
  public function getAssignedTypeText()[]: string;
  public function getName()[]: string;
  public function getFileName()[]: string;
  public function getFile()[]: ReflectionFile;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  final public function getAttributeClass<T as HH\TypeAliasAttribute>(
    class<T> $c,
  )[]: ?T;
}

class ReflectionType implements IPureStringishObject {
  private function __clone(): void;
  public function __construct(
    ?Reflector $param_or_ret = null,
    darray<arraykey, mixed> $type_hint_info = dict[],
  );
  public function allowsNull()[]: bool;
  public function isBuiltin()[]: bool;
  public function __toString()[]: string;
}

class ReflectionFile implements Reflector {
  private function __clone(): void;
  final public function __construct(string $name)[];
  public function __toString()[]: string;
  public function getName()[]: string;
  public function getModule()[]: ?string;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  final public function getAttributeClass<T as HH\FileAttribute>(
    class<T> $c,
  )[]: ?T;
}

class ReflectionModule implements Reflector {
  private function __clone(): void;
  final public function __construct(string $name)[];
  public function __toString()[]: string;
  public function getName()[]: string;
  final public function getAttributes()[]: darray<string, varray<mixed>>;
  final public function hasAttribute(string $name)[]: bool;
  final public function getAttribute(string $name)[]: ?varray<mixed>;
  final public function getAttributeClass<T as HH\ModuleAttribute>(
    class<T> $c,
  )[]: ?T;
}
