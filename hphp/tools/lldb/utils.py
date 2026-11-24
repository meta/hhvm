# Copyright 2022-present Facebook. All Rights Reserved

# pyre-strict

import abc
import argparse
import collections
import collections.abc
import enum
import functools
import re
import shlex
import struct
import sys
import time
import typing

import lldb


# Setting the values strings makes adding them
# as choices in the LLVMVersion set command subparser work with
# and unnecessary casting
class LLVMVersion(enum.Enum):
    LLVM15 = "15"
    LLVM19 = "19"
    LLVM21 = "21"


_LLVMVersion: LLVMVersion | None = None


class Command(abc.ABC):
    """The name used to call the command"""

    command: str

    """ A short, one-line description of the command """
    description: str

    """ Additional information to display after the usage """
    epilog: typing.Optional[str] = None

    def __init__(self, _debugger: lldb.SBDebugger, _internal_dict) -> None:
        self.parser: argparse.ArgumentParser = self.create_parser()
        self.help_string: str = self.parser.format_help()

    @classmethod
    def register_lldb_command(
        cls,
        debugger: lldb.SBDebugger,
        module_name: str,
        top_module: str = "",
    ) -> None:
        parser = cls.create_parser()
        cls.__doc__ = parser.format_help()
        command = f"command script add -o -c {top_module + '.' if top_module else ''}{module_name}.{cls.__name__} {cls.command}"
        debugger.HandleCommand(command)

    @classmethod
    def default_parser(cls) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser(
            description=cls.description,
            prog=cls.command,
            epilog=cls.epilog,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        return parser

    def get_short_help(self) -> str:
        return self.description

    def get_long_help(self) -> str:
        return self.help_string

    @classmethod
    @abc.abstractmethod
    def create_parser(cls) -> argparse.ArgumentParser:
        """Create and return an ArgumentParser object.

        Typical usage is to call .add_argument()
        as needed on the parser that default_parser() returns,
        and return that parser.
        """
        ...

    @abc.abstractmethod
    def __call__(
        self,
        debugger: lldb.SBDebugger,
        command: str,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
    ) -> None: ...


def get_llvm_version(target: lldb.SBTarget) -> LLVMVersion:
    global _LLVMVersion
    if _LLVMVersion is not None:
        return _LLVMVersion

    found_version = None
    try:
        hhvm_module = get_hhvm_module(target)
        assert hhvm_module is not None
        section = hhvm_module.FindSection(".comment")
        size = section.GetFileByteSize()
        data = section.GetSectionData()
        err = lldb.SBError()
        i = 0
        # Older clang versions can appear alongside the one the hhvm binary
        # was built with, so take the first newer version we recognize.
        while i < size:
            s = data.GetString(err, i)
            if err.Fail():
                debug_print(
                    f"Failed to get string from '.comment' section at offset {i}: {err}"
                )
                break
            if re.search("clang version 21", s):
                found_version = LLVMVersion.LLVM21
                break
            if re.search("clang version 19", s):
                found_version = LLVMVersion.LLVM19
                break
            i += len(s) + 1
    except Exception as e:
        # We tried our best, let's assume we're on predetermined version
        debug_print(f"Failed in get_llvm_version: {str(e)}")

    if found_version is None:
        found_version = LLVMVersion.LLVM21
        print(f"Unable to determine LLVM version, assuming it's {LLVMVersion.LLVM21}")

    _LLVMVersion = found_version
    return _LLVMVersion


class LLVMVersionCommand(Command):
    command = "llvm-version"
    description = (
        "Lookup or explicitly set the LLVM version used to build the HHVM binary"
    )

    @classmethod
    def create_parser(cls) -> argparse.ArgumentParser:
        parser = cls.default_parser()
        subparsers = parser.add_subparsers(dest="cmd")
        subparsers.add_parser(
            "get", help="Look up the LLVM version used to build the HHVM binary"
        )
        set_parser = subparsers.add_parser(
            "set",
            help="Set the LLVM version (used by the helper scripts for looking up types)",
        )
        set_parser.add_argument("version", choices=[v.value for v in LLVMVersion])
        return parser

    def __call__(
        self,
        debugger: lldb.SBDebugger,
        command: str,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
    ) -> None:
        command_args = shlex.split(command)
        try:
            options = self.parser.parse_args(command_args)
        except SystemExit:
            result.SetError("option parsing failed")
            return

        if options.cmd == "get":
            result.write(get_llvm_version(exe_ctx.target).value)
        elif options.cmd == "set":
            global _LLVMVersion
            _LLVMVersion = LLVMVersion(options.version)
        else:
            result.SetError(f"Unexpected command {options.cmd}")


# -------------------------------------------------------------------------------------
# Memoization for functions that take in hashable arguments (i.e. not lldb.SB* values).

_all_caches: typing.List[typing.Dict[typing.Any, typing.Any]] = []


def memoized(
    func: typing.Callable[..., typing.Any],
) -> typing.Callable[..., typing.Any]:
    """Simple memoization decorator that ignores **kwargs."""
    global _all_caches

    cache: typing.Dict[typing.Any, typing.Any] = {}
    _all_caches.append(cache)

    @functools.wraps(func)
    def memoizer(*args: typing.Any) -> typing.Any:
        if not isinstance(args, collections.abc.Hashable):
            return func(*args)
        if args not in cache:
            cache[args] = func(*args)
        return cache[args]

    return memoizer


# ------------------------------------------------------------------------------
# Type and symbol lookup
#
# Note that we need to thread a lldb.Target around, unless
# we want to store it globally somewhere, because lldb.target returns None
# when within a script. I.e. the target object needs to come from the
# execution context that is passed when starting a command's execution.

_target_cache: typing.Dict[int, typing.Dict[str, typing.Any]] = {}


def clear_caches(
    target: typing.Optional[lldb.SBTarget] = None, key: typing.Optional[str] = None
) -> None:
    if target is None:
        _target_cache.clear()
        for cache in _all_caches:
            cache.clear()
        return

    target_idx = target.GetDebugger().GetIndexOfTarget(target)
    if target_idx in _target_cache:
        cache = _target_cache[target_idx]
        if key is not None:
            if key in cache:
                del cache[key]
        else:
            del _target_cache[target_idx]


def get_target_cache_dict(target: lldb.SBTarget) -> typing.Dict[str, typing.Any]:
    """Get the target cache dictionary for the specified target"""

    target_idx = target.GetDebugger().GetIndexOfTarget(target)

    if target_idx not in _target_cache:
        _target_cache[target_idx] = {}

    return _target_cache[target_idx]


def get_hhvm_module(target: lldb.SBTarget) -> typing.Optional[lldb.SBModule]:
    """Get the module that contains the HHVM globals and types"""

    target_cache_dict = get_target_cache_dict(target)
    key = "module"

    if key in target_cache_dict:
        return target_cache_dict[key]

    for module in target.modules:
        # Pick a symbol name we can rely on to always find the HHVM module here
        sym = module.FindSymbol("HPHP::jit::tc::g_code")
        if sym is not None and sym.IsValid():
            target_cache_dict[key] = module
            debug_print(
                f"get_hhvm_module({target}): found hhvm module {module} (uuid {module.GetUUIDString()})"
            )
            return module

    return None


def get_cached_type(name: str, target: lldb.SBTarget) -> typing.Optional[lldb.SBType]:
    """Get a type by name (trying from the HHVM module first) and cache the results"""

    target_cache_dict = get_target_cache_dict(target)
    key = "types"

    if key not in target_cache_dict:
        target_cache_dict[key] = {}
    elif name in target_cache_dict[key]:
        return target_cache_dict[key][name]  # Return the cached type

    ty = None

    hhvm_module = get_hhvm_module(target)
    if hhvm_module is not None:
        ty = hhvm_module.FindFirstType(name)

        # There was a bug in FindFirstType (T133615659) that has been fixed. But just in case...
        if ty is None or not ty.IsValid():
            ty = hhvm_module.FindTypes(name).GetTypeAtIndex(0)

    # If we can't find it in the hhvm module,
    # let's try it on the target, which might take longer but handles cases where
    # the type we want is in a different module. Looking in the hhvm module first
    # lets us prioritize types defined in it.
    if ty is None or not ty.IsValid():
        ty = target.FindFirstType(name)
        if ty is None or not ty.IsValid():
            ty = target.FindTypes(name).GetTypeAtIndex(0)

    if ty is not None and ty.IsValid():
        target_cache_dict[key][name] = ty

    return ty


def get_cached_global(
    name: str,
    target: lldb.SBTarget,
) -> typing.Optional[lldb.SBValue]:
    """Get a global by name (trying from the HHVM module first) and cache the results"""
    debug_print(f"get_cached_global({name})")

    target_cache_dict = get_target_cache_dict(target)
    key = "globals"

    if key not in target_cache_dict:
        target_cache_dict[key] = {}
    elif name in target_cache_dict[key]:
        return target_cache_dict[key][name]  # Return the cached global

    g = None

    hhvm_module = get_hhvm_module(target)
    if hhvm_module is not None:
        g = hhvm_module.FindFirstGlobalVariable(target, name)
        if g is None or g.GetError().Fail():
            g = hhvm_module.FindGlobalVariables(target, name, 1).GetValueAtIndex(0)

    if g is None or g.GetError().Fail():
        g = target.FindFirstGlobalVariable(name)
        if g is None or g.GetError().Fail():
            g = target.FindGlobalVariables(name, 1).GetValueAtIndex(0)

    if g is None or g.GetError().Fail():
        debug_print(
            f"couldn't find global variable '{name}'; attempting to find it by evaluating it"
        )
        g = Value(name, target)

    target_cache_dict[key][name] = g
    return g


def Type(name: str, target: lldb.SBTarget) -> lldb.SBType:
    """Look up an HHVM type

    Raises an exception if the type cannot be found.

    Arguments:
        name: name of the type
        targ: optional target from the execution context

    Returns:
        An SBType wrapping the HHVM type
    """
    debug_print(f"Type({name}, {target})")
    ty = get_cached_type(name, target)
    assert ty is not None and ty.IsValid(), f"couldn't find type '{name}'"
    return ty


def Global(name: str, target: lldb.SBTarget) -> lldb.SBValue:
    """Look up the value of a global variable (including static)

    Falls back to evaluating the variable as an expression, which
    raises an exception on failure.

    Arguments:
        name: name of the global variable
        target: target from the execution context

    Returns:
        SBValue wrapping the global variable
    """
    debug_print(f"Global({name}, {target})")
    g = get_cached_global(name, target)
    assert g is not None and g.GetError().Success(), f"couldn't find global '{name}'"
    return g


def Enum(
    enum_name: str,
    elem: typing.Union[str, int],
    target: lldb.SBTarget,
) -> lldb.SBTypeEnumMember:
    """Look up the value of an enum member

    Raises an exception if the enum or member cannot be found.

    Arguments:
        enum_name: name of the enumeration
        elem: name or index of the enumerator element
        target: target from the execution context

    Returns:
        SBTypeEnumMember wrapping the enumerator element
    """
    debug_print(f"Enum({enum_name}, {elem}, {target})")
    enum = Type(enum_name, target)
    assert enum.IsValid(), f"couldn't find enumeration '{enum_name}'"
    members = enum.GetEnumMembers()
    assert members.IsValid(), f"'{enum_name} is not an enumeration"
    val = members[elem]
    assert val is not None and val.IsValid(), (
        f"couldn't find enumerator '{elem}' in '{enum_name}'"
    )
    return val


def Value(name: str, target: lldb.SBTarget) -> lldb.SBValue:
    """Look up the value of a symbol, by evaluating it as an expression

    Raises an exception if the symbol cannot be found/expression evaluated.
    You typically should call Global(), which calls this as a fallback.

    Arguments:
        name: the symbol to evaluate
        target: target from the execution context

    Returns:
        SBValue wrapping the value
    """
    debug_print(f"Value({name}, {target})")
    v = target.EvaluateExpression(name)
    assert v.GetError().Success(), f"couldn't find symbol {name}"
    return v


# ------------------------------------------------------------------------------
# STL accessors


def atomic_get(atomic: lldb.SBValue) -> lldb.SBValue:
    inner = rawtype(atomic.type).GetTemplateArgumentType(0)

    # Newer lldb/libc++abi toolchains (clang 21) ship a synthetic child
    # provider for std::atomic<T>. Its synthetic view only exposes the
    # std::__atomic_base<T> base class as a child, so looking up the raw
    # storage member (_M_i / _M_b) by name fails. Drop to the non-synthetic
    # value so we can reach the underlying std::__atomic_base members.
    atomic = atomic.GetNonSyntheticValue()

    if inner.IsPointerType():
        return get(atomic, "_M_b", "_M_p")
    else:
        return get(atomic, "_M_i")


# ------------------------------------------------------------------------------
# Member access helpers.


def get(struct: lldb.SBValue, *field_names: str) -> lldb.SBValue:
    """Get the value of struct.field_names[0][field_names[1]]...

    Arguments:
        struct: The struct to reach into
        field_names: Name of the fields to extract. If more than one field name is
                     given, then the rest are treated as child members such that this
                     function is called recursively.

    Returns:
        The value of the field. Throws an assertion is anything goes wrong
        in the retrieval.

    This is supposed to be semi-equivalent to gdb's struct[field_name] syntax.
    """

    assert struct.GetError().Success(), f"invalid struct '{struct.name}'"
    # Note: You can also do lldb.value(val).<name>
    v = struct.GetChildMemberWithName(field_names[0])
    assert v.GetError().Success(), (
        f"couldn't find field '{field_names[0]}' in struct '{struct.name}' with type '{struct.type.name}'"
    )

    if len(field_names) == 1:
        return v
    else:
        return get(v, *field_names[1:])


# ------------------------------------------------------------------------------
# Type manipulations


def rawtype(t: lldb.SBType) -> lldb.SBType:
    """Remove const, volatile, and typedefs

    Arguments:
        t: type to translate

    Returns:
        An SBType object with qualifiers and layers of typedefs removed

    For examples:
        typedef HPHP::Native::NativeDataInfo::InitFunc ... -> void (*)(HPHP::ObjectData *)
        const HPHP::Class * -> HPHP::Class *
    """
    # NOTE: It appears GetCanonicalType() does the same thing
    #       as GetTypedefedType() for our purposes.
    #       Also look at GetBasicType().
    return t.GetUnqualifiedType().GetCanonicalType()


def destruct(t: str) -> str:
    """Drop the class-key from a type name"""
    return re.sub(r"^(struct|class|union)\s+", "", t)


def template_type(t: lldb.SBType) -> str:
    """Get the unparameterized name of a template type.

    Arguments:
        t: type to drop template parameters from

    Return:
        The unparameterized name

    For example:
        HPHP::VMFixedVector<ObjectProps::quick_index> -> 'HPHP::VMFixedVector'
    """
    return destruct(rawtype(t).name.split("<")[0])


def unsigned_cast(v: lldb.SBValue, t: lldb.SBType) -> lldb.SBValue:
    """Perform a cast of `v` to `t` with C compatible unsigned widening.

    Use this in place of SBValue::Cast for cases where you are widening a
    value (such as lowptr -> ptr conversion).  SBValue::Cast performs only a
    reinterpretation of the bits so for a widening conversion it potentially
    add garbage in the upper bits.

    Arguments:
        v: the value to cast

    Return:
        The value v casted to type `t` using C style semantics unsigned
        widening.
    """

    assert not v.type.IsAggregateType(), "Cannot cast aggregate type"
    assert v.GetByteSize() <= 8, "Value is too wide"

    # In theory we should use `v.path` to access value v in an expression.  In
    # practice this doesn't work because there are bugs in lldb.  Such as the
    # one that was being fixed in https://reviews.llvm.org/D132734,
    # which is reverted.  If this worked properly we could have this cast
    # operation follow C style semantics according to the types in play (as
    # opposed to forcing no sign extension).
    debug_print(f"unsigned_cast(v=0x{v.unsigned:x}) {t.name}")
    ret = v.target.EvaluateExpression(f"({t.name}){v.unsigned}")
    debug_print(f"unsigned_cast(v=0x{v.unsigned:x}) {t.name} res {ret.unsigned:x}")

    assert ret is not None and ret.GetError().Success(), (
        f"Failed to cast {v} ({v.unsigned}) to {t.name}"
    )
    return ret


# ------------------------------------------------------------------------------
# Pointer helpers


def nullptr(target: lldb.SBTarget) -> lldb.SBValue:
    """Return an SBValue wrapping a pointer to 0"""
    return target.CreateValueFromExpression("nullptr", "(void *)0")


def is_nullptr(val: lldb.SBValue) -> bool:
    return val.TypeIsPointerType() and val.unsigned == 0


def referenced_value(val: lldb.SBValue) -> lldb.SBValue:
    """Get the value referenced by a pointer/reference, or the value itself otherwise"""
    if val.type.IsReferenceType() or val.type.IsPointerType():
        return val.Dereference()
    return val


def rawptr(val: lldb.SBValue) -> typing.Optional[lldb.SBValue]:
    """Fully strip a smart pointer to a raw pointer. References are re-cast as pointers.

    Arguments:
        val: A smart pointer

    Returns:
        The stripped pointer, or None if it's not yet supported
    """
    # debug_print(f"rawptr(val=0x{val.unsigned:x})")

    if val.type.IsPointerType():
        debug_print(f"rawptr(val=0x{val.unsigned:x}) is pointer type")
        return val
    elif val.type.IsReferenceType():
        debug_print(f"rawptr(val=0x{val.unsigned:x}) is reference type")
        return referenced_value(val)

    name = template_type(val.type)
    ptr = None

    debug_print(f"rawptr(val=0x{val.unsigned:x}) template {name}")
    if name == "std::unique_ptr":
        # This is a synthetic value, so we can just use its synthesized child.
        # We could also do utils.get(val.GetNonSyntheticValue(), "_M_t", "_M_t", "_M_head_impl")
        ptr = get(val, "pointer")
    elif name == "HPHP::req::ptr" or name == "HPHP::AtomicSharedPtrImpl":
        ptr = get(val, "m_px")
    elif name == "HPHP::LowPtr" or name == "HPHP::detail::LowPtrImpl":
        inner = val.type.GetTemplateArgumentType(0)
        ptr = get(val, "m_s")
        if name == "HPHP::detail::LowPtrImpl":
            storage_type = template_type(val.type.GetTemplateArgumentType(1))
            # Currently only used by AtomicLowPtr alias
            assert storage_type == "HPHP::detail::AtomicStorage"
            ptr = get(ptr, "_M_i")
        ptr = unsigned_cast(ptr, inner.GetPointerType())
    elif name == "HPHP::ptrimpl::PtrImpl":
        inner = val.type.GetTemplateArgumentType(0)
        storage = template_type(val.type.GetTemplateArgumentType(1))
        formatting = template_type(val.type.GetTemplateArgumentType(2))
        ptr = get(val, "m_s")
        debug_print(f"rawptr(val=0x{ptr.unsigned:x}) template {storage} {formatting}")
        if storage == "HPHP::ptrimpl::Atomic":
            # m_s is a std::atomic<T>; use atomic_get so we correctly read the
            # raw storage even when lldb provides a synthetic std::atomic view.
            ptr = atomic_get(ptr)
            debug_print(f"rawptr(val=0x{ptr.unsigned:x}) read atomic")

        if formatting == "HPHP::ptrimpl::UInt32Packed":
            addr = ptr.unsigned << 3
            ptr = val.CreateValueFromExpression(
                "(tmp)", f"({inner.GetPointerType()}) {addr}"
            )
            debug_print(f"rawptr(val=0x{ptr.unsigned:x}) read packed")
        else:
            debug_print(
                f"rawptr(val=0x{ptr.unsigned:x}) start non packed: ({inner.GetPointerType()}) {ptr.unsigned:x}"
            )
            ptr = unsigned_cast(ptr, inner.GetPointerType())
            debug_print(f"rawptr(val=0x{ptr.unsigned:x}) end non packed")
        debug_print(f"rawptr(val=0x{val.unsigned:x}) ptrimpl done {ptr.unsigned:x}")
    elif name == "HPHP::CompactTaggedPtr":
        inner = val.type.GetTemplateArgumentType(0)
        addr = get(val, "m_data").unsigned & 0xFFFFFFFFFFFF
        ptr = val.CreateValueFromExpression(
            "(tmp)", f"({inner.GetPointerType()}) {addr}"
        )
    elif name == "HPHP::CompactSizedPtr":
        ptr = rawptr(get(val, "m_data"))
    elif name == "HPHP::LockFreePtrWrapper":
        ptr = rawptr(get(val, "val"))
    elif name == "HPHP::TokenOrPtr":
        ptr = rawptr(TokenOrPtr.get_ptr(val))

    if ptr is not None:
        return rawptr(ptr)

    return None


kMaxTagSize: int = 16
kShiftAmount: int = 64 - kMaxTagSize  # 64 = std::numeric_limits<uintptr_t>::digits


class TokenOrPtr:
    @staticmethod
    def get_compact(token_or_ptr: lldb.SBValue) -> int:
        compact = get(token_or_ptr, "m_compact")  # HPHP::CompactTaggedPtr
        data = compact.unsigned >> 2
        return data

    @staticmethod
    def get_tag(token_or_ptr: lldb.SBValue) -> int:
        data = TokenOrPtr.get_compact(token_or_ptr)
        tag = data >> kShiftAmount
        return tag

    @staticmethod
    def is_ptr(token_or_ptr: lldb.SBValue) -> bool:
        return TokenOrPtr.get_tag(token_or_ptr) == 0

    @staticmethod
    def is_token(token_or_ptr: lldb.SBValue) -> bool:
        return TokenOrPtr.get_tag(token_or_ptr) == 1

    @staticmethod
    def get_ptr(token_or_ptr: lldb.SBValue) -> lldb.SBValue:
        data = TokenOrPtr.get_compact(token_or_ptr)
        ptr = data & (-1 >> kMaxTagSize)
        return token_or_ptr.CreateValueFromExpression("tmp", f"(uintptr_t *){ptr}")

    @staticmethod
    def get_token(token_or_ptr: lldb.SBValue) -> lldb.SBValue:
        ptr = TokenOrPtr.get_ptr(token_or_ptr)
        repo_token_type = Type("HPHP::RepoFile::Token", token_or_ptr.target)
        return unsigned_cast(ptr, repo_token_type)


def deref(val: lldb.SBValue) -> lldb.SBValue:
    """Fully dereference a value, stripping away *, &, and all known smart
    pointer wrappers (as well as const/volatile qualifiers).

    Arguments:
        val: The value to dererefence

    Returns:
        The fully dereferenced value.
    """
    p = rawptr(val)

    if p is None:
        return val.Cast(rawtype(val.type))
    else:
        return deref(referenced_value(p))


def ptr_add(
    ptr: lldb.SBValue, n: typing.Union[int, lldb.SBValue], sizeof: bool = True
) -> lldb.SBValue:
    """Create new a new pointer, pointing to value of ptr+(n*sizeof(*ptr))

    When sizeof=False, just add n, i.e. treating sizeof(*ptr) == 1

    Example:
        >>> pc
        (HPHP::PC) bc = 0x00007ffff1c00b00 "\U00000014\U00000003\U00000010"
        >>> pc1 = utils.ptr_add(pc, 1)
        >>> pc1
        (HPHP::PC) pc+1 = 0x00007ffff1c00b01 "\U00000003\U00000010"
    """

    assert ptr.TypeIsPointerType(), f"Expected ptr type, got {ptr.type.name}"

    if isinstance(n, lldb.SBValue):
        n = n.unsigned
    if sizeof:
        n *= ptr.type.GetPointeeType().size

    # TODO(michristensen) Creating the new pointer using the following two
    # commented-out lines was adding extraneous MSB bytes in some cases:
    # data = lldb.SBData.CreateDataFromInt(ptr.unsigned + n)
    # val = ptr.CreateValueFromData(f"{ptr.name}+{str(n)}", data, ptr.type)
    val = ptr.CreateValueFromExpression(
        f"{ptr.name}+{str(n)}"[-10:], f"({ptr.type.name}){ptr.unsigned + n}"
    )
    return val


# ------------------------------------------------------------------------------
# Name accessor


def _full_func_name(func: lldb.SBValue) -> str:
    attrs = atomic_get(get(func, "m_attrs", "m_attrs"))
    if attrs.unsigned & Enum("HPHP::Attr", "AttrIsMethCaller", func.target).unsigned:
        cls = ""
    else:
        # m_impl is an AtomicPackedPtr<Class>; use rawptr so the packed
        # storage is unpacked (shifted) into a real Class pointer, rather than
        # atomic_get which would only return the raw packed storage value.
        cls = rawptr(get(func, "m_u", "m_cls", "m_impl"))
        if cls is None or cls.unsigned == 0:
            cls = ""
        else:
            cls = (
                # pyre-fixme[58]: `+` is not supported for operand types
                #  `Optional[str]` and `str`.
                nameof(
                    unsigned_cast(cls, Type("HPHP::Class", cls.target).GetPointerType())
                )
                + "::"
            )
    return cls + string_data_val(deref(get(func, "m_name")))


def nameof(val: lldb.SBValue) -> typing.Optional[str]:
    """Get the name of various HPHP objects, like functions or classes.

    Arguments:
        val: the value to get the name of (a Func, Class, or ObjectData)
    Returns:
        A Python string, if there is name associated with the object; otherwise None
    """
    val = deref(val)
    debug_print(f"nameof(val=0x{val.load_addr:x} (type={val.type.name}))")
    try:
        t = val.type.name
    except Exception:
        return None

    sd = None

    def _od_name(od: lldb.SBValue) -> lldb.SBValue:
        cls = deref(get(od, "m_cls"))
        pre_class = deref(get(cls, "m_preClass"))
        sd = get(pre_class, "m_name")
        return sd

    if t == "HPHP::Func":
        sd = get(val, "m_fullName")
        v = rawptr(sd)
        assert v is not None
        # When the full name still needs to be computed from the class and
        # method names, m_fullName holds the kNeedsFullName sentinel. Since
        # D-"Make kNeedsFullName less magical", that sentinel is the max value
        # of the packed storage (uint32_t all-ones); AtomicPackedPtr stores it
        # raw and rawptr() surfaces it shifted left by the packed pointer shift
        # of 3 (see UInt32Packed in hphp/util/ptr-impl.h). Older non-packed
        # builds used a raw sentinel value of 1.
        if v.unsigned == 1 or (v.unsigned >> 3) == 0xFFFFFFFF:
            return _full_func_name(val)
    elif t == "HPHP::Class":
        pre_class = deref(get(val, "m_preClass"))
        sd = get(pre_class, "m_name")
    elif t == "HPHP::LazyClassData":
        sd = get(val, "className")
    elif t == "HPHP::ObjectData":
        sd = _od_name(val)
    elif t == "HPHP::Object":
        sd = _od_name(deref(get(val, "m_obj")))

    if sd is None:
        return None

    return string_data_val(deref(sd))


# ------------------------------------------------------------------------------
# Intel CRC32


def _bit_reflect(num: int, nbits: int) -> int:
    """Perform bit reflection on the bottom 'nbits' of 'num'"""

    out = 0
    mask = 1 << (nbits - 1)
    for i in range(nbits):
        if num & (1 << i):
            out |= mask
        mask >>= 1
    return out


def crc32q(crc: int, quad: int) -> int:
    """Intel SSE4 CRC32 implementation"""

    crc = _bit_reflect(crc, 32)
    quad = _bit_reflect(quad, 64)

    msb = 1 << 63

    dividend = quad ^ (crc << 32)
    divisor = 0x11EDC6F41 << 31

    for _ in range(64):
        if dividend & msb:
            dividend ^= divisor
        dividend <<= 1

    return _bit_reflect(dividend, 64)


# ------------------------------------------------------------------------------
# String helpers


def read_cstring(
    addr: typing.Union[int, lldb.SBValue],
    len: int,
    process: lldb.SBProcess,
    keep_case: bool = True,
) -> str:
    """Read a null-terminated char * from memory at the given address or lldb.SBValue's load_addr

    LLDB can already format char * variables, like so:

        (char *) varname = 0x0000abcd "string value"

    This gets just the "string value" char array.
    """
    # If it's already a known char *, just parse its default LLDB summary,
    # which appears to be of the form '"some_str"'.
    if isinstance(addr, lldb.SBValue):
        if rawtype(addr.type).name == "char *":
            s = addr.summary
            if s and s[0] == '"':
                s = s[1:]
            if s and s[-1] == '"':
                s = s[:-1]
            if s:
                return s
            else:
                # Empty string, so let's try reading it from memory
                addr = addr.unsigned
        else:
            addr = addr.load_addr

    err = None

    try:
        error = lldb.SBError()
        cstring = process.ReadCStringFromMemory(addr, len, error)
        if error.Success():
            # pyre-fixme[16]: ReadCStringFromMemory returns str at runtime
            return cstring if keep_case else cstring.lower()
        else:
            err = error
    except SystemError as error:
        err = error

    print(f"error while trying to get string: {err}", file=sys.stderr)
    return f"<invalid string with addr 0x{addr:0x} and length {len}>"


def _unpack(s: str) -> int:
    return 0xDFDFDFDFDFDFDFDF & struct.unpack("<Q", bytes(s, encoding="utf-8"))[0]


def hash_string(s: str) -> int:
    """Hash a string as in hphp/util/hash-crc-x64.S"""

    size = len(s)
    tail_sz = size % 8
    size -= tail_sz

    crc = 0xFFFFFFFF

    for i in range(0, size, 8):
        crc = crc32q(crc, _unpack(s[i : i + 8]))

    if tail_sz == 0:
        return crc >> 1

    shift = -((tail_sz - 8) << 3) & 0b111111
    tail = _unpack(s[size:].ljust(8, "\0"))

    crc = crc32q(crc, tail << shift)
    return crc >> 1


def is_char_pointer_type(t: lldb.SBType, target: lldb.SBTarget) -> bool:
    return (
        re.match(r"char((8|16|32)_t)? \*", t.name) is not None
        or re.match(r"char \[\d*\]$", t.name) is not None
        or t == Type("char", target).GetPointerType()
    )


def strinfo(s: lldb.SBValue, keep_case: bool = True) -> typing.Dict[str, typing.Any]:
    """Return the Python string and HHVM hash for `s`, or None if `s` is not a stringish lldb.Value"""

    data = None
    h = None

    try:
        t = rawtype(s.type)
    except Exception as err:
        print(
            f"error while trying to get the type of what we believe is a string-like value: {err}",
            file=sys.stderr,
        )
        # pyre-fixme[7]: Expected `Dict[str, typing.Any]` but got `None`.
        return None

    if is_char_pointer_type(t, s.target):
        # Note: 1024 is very arbitrary; the string may
        # very well be longer than this.
        data = read_cstring(s.deref.load_addr, 1024, s.process)
    else:
        sd = deref(s)
        ty_name = rawtype(sd.type).name
        if ty_name in ("HPHP::String", "HPHP::StaticString", "HPHP::OptString"):
            # HPHP::OptString is the base of HPHP::String and holds the same
            # m_str (req::ptr<StringData>) member.
            sd = rawptr(get(sd, "m_str"))
            assert sd is not None
            sd = deref(sd)
        elif ty_name == "HPHP::StrNR":
            sd = deref(get(sd, "m_px"))

        if rawtype(sd.type).name != "HPHP::StringData":
            # pyre-fixme[7]: Expected `Dict[str, typing.Any]` but got `None`.
            return None

        data = string_data_val(sd)

        m_hash = get(sd.children[1], "m_hash").signed
        if m_hash != 0:
            h = m_hash & 0x7FFFFFFF

    if data is None:
        # pyre-fixme[7]: Expected `Dict[str, typing.Any]` but got `None`.
        return None

    assert isinstance(data, str)
    retval = {
        "data": data if keep_case else data.lower(),
        "hash": h if h is not None else hash_string(data),
    }
    return retval


def string_data_val(val: lldb.SBValue, keep_case: bool = True) -> str:
    """Convert an HPHP::StringData[*] to a Python string

    Arguments:
        val: A StringData* wrapped by an lldb.SBValue
        keep_case: If False, lowercase the returned string

    Returns:
        A Python string with the string value stored by the StringData
    """

    if val.type.IsPointerType():
        val = deref(val)
    assert val.type.name == "HPHP::StringData", f"invalid type {val.type.name}"

    addr = val.load_addr
    assert addr != lldb.LLDB_INVALID_ADDRESS, (
        f"invalid string address 0x{val.load_addr:x}"
    )
    addr += val.size
    m_len = val.children[1].GetChildMemberWithName("m_len").unsigned
    return read_cstring(addr, m_len + 1, val.process)


# ------------------------------------------------------------------------------
# Resource helpers


def pretty_resource_data(data: lldb.SBValue, print_header: bool = False) -> str:
    """Convert an HPHP::ResourceData[*] to a Python string

    Arguments:
        val: A ResourceData* wrapped by an lldb.SBValue
        print_header: If True, also print the ResourceHeader the precedes it

    Returns:
        A Python string representing the contents of the ResourceData object
    """

    if data.type.IsPointerType():
        data = deref(data)

    assert data.type.name == "HPHP::ResourceData"
    return str(hex(data.load_addr))


def pretty_resource_header(header: lldb.SBValue, print_data: bool = True) -> str:
    """Convert an HPHP::ResourceHdr[*] to a Python string

    Arguments:
        val: A ResourceHdr* wrapped by an lldb.SBValue
        print_data: If True, also print the ResourceData that follows

    Returns:
        A Python string representing the contents of the ResourceHdr object
    """

    if header.type.IsPointerType():
        header = deref(header)

    assert header.type.name == "HPHP::ResourceHdr", (
        f"Expected HPHP::ResourceHdr, got {header.type.name}"
    )
    data = None
    if print_data:
        addr = header.load_addr
        addr += header.size
        data = header.CreateValueFromAddress(
            "data", addr, Type("HPHP::ResourceData", header.target)
        )
        data = pretty_resource_data(data)

    header_str = str(hex(header.load_addr))

    return f"(hdr = {header_str}" + (f", data = {data}" if data else "") + ")"


# ------------------------------------------------------------------------------
# TypedValue helpers


def pretty_tv(typ: lldb.SBValue, data: lldb.SBValue) -> str:
    """Get the pretty string representation of a TypedValue (or its subclasses)

    Note that calling str(val) on an SBValue will automatically use the
    pretty printer for that value, if present.

    Arguments:
        typ: A HPHP::DataType wrapped by an lldb.SBValue
        data: A HPHP::Value wrapped by an lldb.SBValue

    Returns:
        A Python string representing the TypedValue
    """

    target: lldb.SBTarget = typ.target
    typ = typ.Cast(Type("HPHP::DataType", target))

    def DT(elem: typing.Union[str, int]) -> int:
        return Enum("HPHP::DataType", elem, target).unsigned

    val = None
    name = None
    typ_str: str | None = None

    if typ.unsigned in [DT("Uninit"), DT("Null")]:
        pass
    elif typ.unsigned == DT("Boolean"):
        val = get(data, "num").unsigned
        val = bool(val) if val in (0, 1) else val
    elif typ.unsigned == DT("Int64"):
        val = get(data, "num").signed
    elif typ.unsigned == DT("Double"):
        val = float(get(data, "dbl").value)
    elif typ.unsigned == DT("String"):
        pstr = get(data, "pstr")
        val = '"%s"' % string_data_val(pstr)
    elif typ.unsigned in (
        DT("Dict"),
        DT("Vec"),
        DT("Keyset"),
    ):
        val = deref(get(data, "parr"))
    elif typ.unsigned == DT("Object"):
        val = get(data, "pobj")
    elif typ.unsigned == DT("Resource"):
        val = deref(get(data, "pres"))
        val = pretty_resource_header(val)
    elif typ.unsigned == DT("Class"):
        val = get(data, "pclass")
    elif typ.unsigned == DT("LazyClass"):
        val = get(data, "plazyclass")
    elif typ.unsigned == DT("Func"):
        val = get(data, "pfunc")
    elif typ.unsigned == DT("ClsMeth"):
        # For non-lowptr, m_data is a pointer, so try and dereference first
        val = referenced_value(get(data, "pclsmeth", "m_data"))
        cls = get(val, "m_cls")
        func = get(val, "m_func")
        name = f"{nameof(cls)}::{nameof(func)}"
    elif typ.unsigned == DT("RFunc"):
        val = get(data, "prfunc")
        func = get(val, "m_func")
        # arr = get(val, "m_arr")  # TODO use to print array contents
        name = nameof(func)
    elif typ.unsigned == DT("RClsMeth"):
        val = get(data, "prclsmeth")
        cls = get(val, "m_cls")
        func = get(val, "m_func")
        # arr = get(val, "m_arr")  # TODO use to print array contents
        name = f"{nameof(cls)}::{nameof(func)}"
    else:
        typ_as_int8_t = typ.Cast(Type("int8_t", target))
        num = get(data, "num").signed
        typ_str = "Invalid Type (%d)" % typ_as_int8_t.signed
        val = "0x%x" % num

    if typ_str is None:
        typ_str = typ.value

    if val is None:
        out = "{ %s }" % typ_str
    elif name is None:
        out = "{ %s, %s }" % (typ_str, str(val))
    else:
        out = '{ %s, %s ("%s") }' % (typ_str, str(val), name)

    return out


# ------------------------------------------------------------------------------
# Array helpers


def has_array_kind(array_data: lldb.SBValue, *kinds: str) -> bool:
    """Determine if array_data has a particular kind (e.g. 'Vec', 'Keyset', etc.)

    Arguments:
        array_data: an SBValue wrapping an ArrayData
        kinds: one or more ArrayKind members

    Returns:
        True if the array data's kind is one of those specified in kinds
    """

    heap_obj = array_data.children[0].children[0]  # HPHP::HeapObject
    m_kind = get(heap_obj, "m_kind")
    debug_print(f"has_array_kind() with m_kind {m_kind}")

    for kind in kinds:
        kind = Enum(
            "HPHP::ArrayData::ArrayKind", "k" + kind + "Kind", array_data.target
        ).unsigned
        if m_kind.unsigned == kind:
            return True
    return False


def cast_as_specialized_array_data_kind(array_data: lldb.SBValue) -> lldb.SBValue:
    # Currently does *not* take in a pointer to an ArrayData, but rather the value itself
    debug_print(
        f"cast_as_specialized_array_data_kind(array_data=0x{array_data.load_addr:x} (type={array_data.type.name}))"
    )
    heap_obj = array_data.children[0].children[0]  # HPHP::HeapObject
    m_kind = get(heap_obj, "m_kind")
    if has_array_kind(array_data, "Vec"):
        pass
    elif has_array_kind(array_data, "Dict"):
        array_data = array_data.address_of.Cast(
            Type("HPHP::VanillaDict", array_data.target).GetPointerType()
        ).deref
    elif has_array_kind(array_data, "Keyset"):
        array_data = array_data.address_of.Cast(
            Type("HPHP::VanillaKeyset", array_data.target).GetPointerType()
        ).deref
    elif has_array_kind(array_data, "BespokeVec", "BespokeDict", "BespokeKeyset"):
        raise Exception(
            f"Unsupported bespoke array type ('{m_kind}')! Run `expression -R -- {array_data.path}` to see its raw form"
        )
    else:
        raise Exception(
            f"Invalid array type ('{m_kind}')! Run `expression -R -- {array_data.path}` to see its raw form"
        )

    if array_data.GetError().Fail():
        raise Exception(
            f"Unable to properly cast array data to specialized type: {array_data.GetError().GetCString()}"
        )
    return array_data


# ------------------------------------------------------------------------------
# Architecture


def arch(target: lldb.SBTarget) -> str:
    """Get the current architecture (e.g. "x86_64" or "aarch64")"""
    try:
        return target.triple.split("-")[0]
    except Exception:
        # pyre-fixme[7]: Expected `str` but got `None`.
        return None


def arch_regs(target: lldb.SBTarget) -> typing.Dict[str, str]:
    """Get a mapping from common register names to the actual register names
        as used in either x86 or ARM

    Arguments:
        target: the target associated with the current debugging session

    Returns:
        A mapping from register name to register name
    """
    a = arch(target)

    if a == "aarch64":
        #  str, str, List[str]]]`.
        return {
            "fp": "fp",
            "sp": "sp",
            "ip": "pc",
            # pyrefly: ignore [bad-assignment]
            "cross_jit_save": [
                "x19",
                "x20",
                "x21",
                "x22",
                "x23",
                "x24",
                "x25",
                "x26",
                "x27",
                "x28",
                "d8",
                "d9",
                "d10",
                "d11",
                "d12",
                "d13",
                "d14",
                "d15",
            ],
        }
    else:
        #  str, str, List[str]]]`.
        return {
            "fp": "rbp",
            "sp": "rsp",
            "ip": "rip",
            # pyrefly: ignore [bad-assignment]
            "cross_jit_save": ["rbx", "r12", "r13", "r14", "r15"],
        }


def reg(common_name: str, frame: lldb.SBFrame) -> lldb.SBValue:
    """Get the value of a register given its common name (e.g. "fp", "sp", etc.)

    Arguments:
        name: Name of the register
        frame: Current frame

    Returns:
        The value of the register, wrapped in a lldb.SBValue. If unrecognized,
        the returned SBValue will be invalid (check with .isValid()).
    """
    name = arch_regs(frame.thread.process.target)[common_name]
    return frame.register[name]


# ------------------------------------------------------------------------------
# General-purpose helpers


def parse_argv(
    args: str, target: lldb.SBTarget, limit: typing.Optional[int] = None
) -> typing.List[lldb.SBValue]:
    """Explode a LLDB argument string, then evaluate all args up to `limit`.

    It assumes that each arg is space-separated, meaning e.g. "bc+bclen" is one argument,
    but "bc + bclen" is three.
    """

    # I can't figure out a way to successfully catch lldb parse errors using normal
    # Python exception handling, so this is best effort.

    if limit is None:
        limit = len(args)
    # pyre-fixme[7]: Beyond limit, args remain as strings
    return [
        target.EvaluateExpression(arg) if i < limit else arg
        for i, arg in enumerate(shlex.split(args))
    ]


# ------------------------------------------------------------------------------
# Debugging

_Debug: bool = False


class DebugCommand(Command):
    command = "debug"
    description = "Enable/disable printing information to aid in debugging LLDB scripts"

    @classmethod
    def create_parser(cls) -> argparse.ArgumentParser:
        parser = cls.default_parser()
        subparsers = parser.add_subparsers(dest="cmd")
        subparsers.add_parser("on", help="Enable LLDB script debugging information")
        subparsers.add_parser("off", help="Disable LLDB script debugging information")
        return parser

    def __call__(
        self,
        debugger: lldb.SBDebugger,
        command: str,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
    ) -> None:
        command_args = shlex.split(command)
        try:
            options = self.parser.parse_args(command_args)
        except SystemExit:
            result.SetError("option parsing failed")
            return

        global _Debug
        if options.cmd == "on":
            _Debug = True
        elif options.cmd == "off":
            _Debug = False
        else:
            result.SetError(f"Unexpected command {options.cmd}")


def debug_print(message: str, file: typing.TextIO = sys.stderr) -> None:
    if _Debug:
        print(message)


def timer(func: typing.Callable[..., typing.Any]) -> typing.Callable[..., typing.Any]:
    @functools.wraps(func)
    def wrapper(*args: typing.Any, **kwargs: typing.Any) -> typing.Any:
        tic = time.perf_counter()
        value = func(*args, **kwargs)
        toc = time.perf_counter()
        elapsed_time = toc - tic
        print(
            f"Elapsed time: %s.%s {elapsed_time:0.4f} seconds"
            % (func.__module__, func.__name__)
        )
        return value

    return wrapper


def __lldb_init_module(debugger: lldb.SBDebugger, top_module: str = "") -> None:
    """Register the commands in this file with the LLDB debugger.

    Defining this in this module (in addition to the main hhvm module) allows
    this script to be imported into LLDB separately; LLDB looks for a function with
    this name at module load time.

    Arguments:
        debugger: Current debugger object

    Returns:
        None
    """
    DebugCommand.register_lldb_command(debugger, __name__, top_module)
    LLVMVersionCommand.register_lldb_command(debugger, __name__, top_module)
