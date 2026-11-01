# pyre-strict
import argparse
import shlex
import typing

import lldb

try:
    # LLDB needs to load this outside of the usual Buck mechanism
    # pyre-fixme[21]: Could not find module `idx`.
    import idx

    # pyre-fixme[21]: Could not find module `sizeof`.
    import sizeof

    # pyre-fixme[21]: Could not find module `unit`.
    import unit

    # pyre-fixme[21]: Could not find module `utils`.
    import utils
except ModuleNotFoundError:
    import hphp.tools.lldb.idx as idx
    import hphp.tools.lldb.sizeof as sizeof
    import hphp.tools.lldb.unit as unit
    import hphp.tools.lldb.utils as utils


def lookup_func(func_id: lldb.SBValue) -> typing.Optional[lldb.SBValue]:
    """Find the function corresponding to a given FuncID

    Args:
        func_id: A HPHP::FuncId wrapped in an lldb.SBValue

    Returns:
        func: A HPHP::Func* wrapped in an lldb.SBValue
    """

    target = func_id.target
    assert func_id.type.name == "HPHP::FuncId", (
        f"invalid func_id, type given is {func_id.type.name} but expected HPHP::FuncId"
    )

    # If we fail to find this symbol, we're going to assume
    # failure is because it actually can't be found (which is the
    # case in lowptr mode since its conditionally compiled in) and
    # not because of some unrelated LLDB error.
    try:
        func_vec = utils.Global("HPHP::Func::s_funcVec", target)
    except Exception:
        func_vec = None

    if func_vec:
        # Non-LowPtr
        utils.debug_print(
            f"lookup_func(func_id={func_id.signed}): identified we're in non-lowptr mode"
        )
        func_id_val = utils.get(func_id, "m_id").unsigned
        result = idx.atomic_low_ptr_vector_at(func_vec, func_id_val)
    else:
        # LowPtr
        utils.debug_print(
            f"lookup_func(func_id={func_id.signed}): identified we're in lowptr mode"
        )
        result = utils.rawptr(utils.get(func_id, "m_id"))

    if result is None:
        return None
    func_ptr = result.Cast(utils.Type("HPHP::Func", target).GetPointerType())
    if func_ptr.GetError().Fail():
        return None
    return func_ptr


def lookup_func_from_frame_pointer(fp: lldb.SBValue) -> typing.Optional[lldb.SBValue]:
    """Get the jitted function pointed to by the given frame pointer.

    Args:
        fp: Activation record (HPHP::ActRec)

    Returns:
        func: An SBValue representing a HPHP::Func*
    """
    func_id = utils.get(fp, "m_funcId")
    return lookup_func(func_id)


def load_litstr_from_repo(
    unit_sn: lldb.SBValue, token: lldb.SBValue
) -> typing.Optional[lldb.SBValue]:
    raise NotImplementedError


def lookup_litstr(
    str_id: lldb.SBValue, unit: lldb.SBValue
) -> typing.Optional[lldb.SBValue]:
    """Find the StringData* corresponding to a given Id

    Args:
        str_id: A HPHP::Id wrapped in an lldb.SBValue

    Returns:
        func: A HPHP::StringData* wrapped in an lldb.SBValue
    """
    m_litstrs = utils.get(unit, "m_litstrs")
    count = sizeof.sizeof(m_litstrs)

    if count is None or str_id.signed < 0 or str_id.signed >= count:
        return None

    try:
        elm = idx.compact_vector_at(m_litstrs, str_id.signed)
        token_or_ptr_ty = utils.Type(
            "HPHP::TokenOrPtr<const HPHP::StringData>", str_id.target
        )
        if elm is None:
            return None
        elm = elm.Cast(token_or_ptr_ty)

        if utils.TokenOrPtr.is_ptr(elm):
            ptr = utils.TokenOrPtr.get_ptr(elm)
            sd_type = utils.Type("HPHP::StringData", str_id.target).GetPointerType()
            s = ptr.Cast(sd_type)
            return s
        else:
            assert utils.TokenOrPtr.is_token(elm)
            m_sn = utils.get(unit, "m_sn")
            token = utils.TokenOrPtr.get_token(elm)
            s = load_litstr_from_repo(m_sn, token)
            return s
    except BaseException:
        return None


def lookup_array(array_id: lldb.SBValue) -> lldb.SBValue:
    """Find the ArrayData* corresponding to a given Id

    Args:
        array_id: A HPHP::Id wrapped in an lldb.SBValue

    Returns:
        array: A HPHP::ArrayData* wrapped in an lldb.SBValue
    """
    raise NotImplementedError


# pyrefly: ignore [invalid-inheritance]
class LookupCommand(utils.Command):
    command = "lookup"
    description = "Look up HHVM runtime objects by ID"

    class ArgsNamespace:  # noqa: B903
        # argparse will add attributes to this class
        def __init__(
            self,
            exe_ctx: lldb.SBExecutionContext,
            result: lldb.SBCommandReturnObject,
        ) -> None:
            self.exe_ctx = exe_ctx
            self.result = result

    @classmethod
    def create_parser(cls) -> argparse.ArgumentParser:
        parser = cls.default_parser()
        subparsers = parser.add_subparsers(
            title="List of lookup subcommands", required=True, dest="command"
        )

        func_cmd = subparsers.add_parser(
            "func",
            help="Look up a Func* by its FuncId",
        )
        func_cmd.add_argument(
            "funcid",
            help="A HPHP::FuncId (i.e. int) uniquely identifying a HPHP::Func*",
        )
        func_cmd.set_defaults(func=cls._lookup_func_prep)

        litstr_cmd = subparsers.add_parser(
            "litstr",
            help="Look up a litstr StringData* by its Id and Unit*",
            epilog="If no Unit is given, the current unit (set by `unit`) is used.",
        )
        litstr_cmd.add_argument(
            "id",
            help="The ID of the desired StringData (i.e. an HPHP::Id)",
        )
        litstr_cmd.add_argument(
            "unit",
            nargs="?",
            help="The unit to use",
        )
        litstr_cmd.set_defaults(func=cls._lookup_litstr_prep)

        array_cmd = subparsers.add_parser(
            "array",
            help="Look up a ArrayData* by its Id",
        )
        array_cmd.add_argument(
            "arrayid",
            help="A HPHP::Id (i.e. int) uniquely identifying a HPHP::ArrayData*",
        )
        array_cmd.add_argument(
            "unit",
            nargs="?",
            help="The unit to use",
        )
        array_cmd.set_defaults(func=cls._lookup_array_prep)

        return parser

    def __call__(
        self,
        debugger: lldb.SBDebugger,
        command: str,
        exe_ctx: lldb.SBExecutionContext,
        result: lldb.SBCommandReturnObject,
    ) -> None:
        namespace = self.ArgsNamespace(exe_ctx, result)
        command_args = shlex.split(command)
        try:
            options = self.parser.parse_args(command_args, namespace=namespace)
            # pyrefly: ignore [missing-attribute]
            options.func(options)
        except SystemExit:
            result.SetError("option parsing failed")
            return

    @classmethod
    def _lookup_func_prep(cls, options: argparse.Namespace) -> None:
        func_id_type = utils.Type("HPHP::FuncId", options.exe_ctx.target)
        func_id = options.exe_ctx.frame.EvaluateExpression(options.funcid).Cast(
            func_id_type
        )
        res = lookup_func(func_id)
        if res is None:
            options.result.SetError(
                f"cannot get function identified with FuncId {func_id}"
            )
            return

        options.result.write(str(res))

    @classmethod
    def _lookup_litstr_prep(cls, options: argparse.Namespace) -> None:
        id_type = utils.Type("HPHP::Id", options.exe_ctx.target)
        id_val = options.exe_ctx.frame.EvaluateExpression(options.id).Cast(id_type)

        unit_val = unit.cur_unit
        if options.unit:
            unit_type = utils.Type(
                "HPHP::Unit", options.exe_ctx.target
            ).GetPointerType()
            unit_val = options.exe_ctx.frame.EvaluateExpression(options.unit).Cast(
                unit_type
            )

        if unit_val is None:
            options.result.SetError("no Unit set")
            return

        res = lookup_litstr(id_val, unit_val)
        if res is None:
            options.result.SetError(f"cannot get string identified with Id {id_val}")
            return

        options.result.write(str(res))

    @classmethod
    def _lookup_array_prep(cls, options: argparse.Namespace) -> None:
        id_type = utils.Type("HPHP::Id", options.exe_ctx.target)
        id_val = options.exe_ctx.frame.EvaluateExpression(options.id).Cast(id_type)
        res = lookup_array(id_val)
        if res is None:
            options.result.SetError(f"cannot get array identified with Id {id_val}")
            return

        options.result.write(str(res))


def __lldb_init_module(
    debugger: lldb.SBDebugger,
    top_module: str = "",
) -> None:
    """Register the commands in this file with the LLDB debugger.

    Defining this in this module (in addition to the main hhvm module) allows
    this script to be imported into LLDB separately; LLDB looks for a function with
    this name at module load time.

    Arguments:
        debugger: Current debugger object

    Returns:
        None
    """
    LookupCommand.register_lldb_command(debugger, __name__, top_module)
