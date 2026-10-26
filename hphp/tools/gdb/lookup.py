#!/usr/bin/env python3

"""
GDB commands for various HHVM ID lookups.
"""

from compatibility import *

import gdb
import idx
import unit
from gdbutils import *


# ------------------------------------------------------------------------------
# `lookup' command.


class LookupCommand(gdb.Command):
    """Lookup HHVM runtime objects by ID."""

    def __init__(self):
        super(LookupCommand, self).__init__(
            "lookup", gdb.COMMAND_DATA, gdb.COMPLETE_NONE, True
        )


LookupCommand()

_first_lookup = True


# ------------------------------------------------------------------------------
# `lookup func' command.


def lookup_func(val):
    """
    Args:
        val: gdb.Value[HPHP::FuncId].

    Returns:
        func: gdb.Value[HPHP::Func*]
    """
    funcid = val.cast(T("HPHP::FuncId"))
    try:
        # Not LowPtr
        global _first_lookup
        if _first_lookup:
            gdb.execute("ptype HPHP::Func::s_funcVec", to_string=True)
            gdb.execute("ptype HPHP::Func::s_funcVec", to_string=True)
            gdb.execute("maint flush symbol-cache")
            _first_lookup = False
        result = idx.atomic_low_ptr_vector_at(
            V("HPHP::Func::s_funcVec"), funcid["m_id"]
        )
        return result.cast(T("HPHP::Func").pointer())
    except gdb.MemoryError:
        raise
    except:
        # LowPtr
        return rawptr(funcid["m_id"]).cast(T("HPHP::Func").pointer())


def lookup_func_from_fp(fp):
    """Get the function pointed to by the given frame pointer.

    Args:
        fp: gdb.Value[HPHP::ActRec].

    Returns:
        func: gdb.Value[HPHP::Func*]
    """
    return lookup_func(fp["m_funcId"])


class LookupFuncCommand(gdb.Command):
    """Lookup a Func* by its FuncId."""

    def __init__(self):
        super(LookupFuncCommand, self).__init__("lookup func", gdb.COMMAND_DATA)

    @errorwrap
    def invoke(self, args, from_tty):
        argv = parse_argv(args)

        if len(argv) != 1:
            print("Usage: lookup func <FuncId>")
            return

        gdbprint(lookup_func(argv[0]))


class LookupFuncFunction(gdb.Function):
    def __init__(self):
        super(LookupFuncFunction, self).__init__("lookup_func")

    @errorwrap
    def invoke(self, val):
        return lookup_func(val)


LookupFuncCommand()
LookupFuncFunction()


# ------------------------------------------------------------------------------
# `lookup litstr' command.


def lookup_litstr(litstr_id, u):
    uloff = V("HPHP::kUnitLitstrOffset")

    if litstr_id < uloff:
        u = V("HPHP::LitstrTable::s_litstrTable")
    else:
        litstr_id -= uloff
        u = u.cast(T("HPHP::UnitExtended").pointer())

    val = u["m_namedInfo"]
    # get the base type
    ty = val.type.fields()[0].type
    val = val.address.cast(ty.pointer()).dereference()
    elm = idx.compact_vector_at(val, litstr_id)
    ty = elm.type.template_argument(0)
    return elm.address.cast(ty.pointer()).dereference()


class LookupLitstrCommand(gdb.Command):
    """Lookup a litstr StringData* by its Id and Unit*.

    If no Unit is given, the current unit (set by `unit') is used.
    """

    def __init__(self):
        super(LookupLitstrCommand, self).__init__("lookup litstr", gdb.COMMAND_DATA)

    @errorwrap
    def invoke(self, args, from_tty):
        argv = parse_argv(args)

        if len(argv) == 0 or len(argv) > 2:
            print("Usage: lookup litstr <Id> [Unit*]")
            return

        if len(argv) == 1:
            if unit.curunit is None:
                print("lookup litstr: No Unit set or provided.")
            u = curunit

        u = argv[0].cast(T("HPHP::Unit").pointer())
        litstr_id = argv[1].cast(T("HPHP::Id"))

        litstr = lookup_litstr(litstr_id, u)
        gdbprint(litstr)


LookupLitstrCommand()
