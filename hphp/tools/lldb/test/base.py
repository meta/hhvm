# Copyright 2022-present Facebook. All Rights Reserved.

# pyre-strict

import abc
import os
import pathlib
import typing
import unittest

import hphp.tools.lldb.utils as utils
import lldb
from __manifest__ import fbmake as build_info
from libfb.py.testutil import BaseFacebookTestCase


def get_lldb_object_description(lldb_object: lldb.SBError) -> typing.Optional[str]:
    stream = lldb.SBStream()
    if not lldb_object.GetDescription(stream):
        return None
    return stream.GetData()


def _get_exe_ctx(debugger: lldb.SBDebugger) -> lldb.SBExecutionContext:
    target = debugger.GetSelectedTarget()
    if not target.IsValid():
        return lldb.SBExecutionContext()
    process = target.GetProcess()
    if not process.IsValid():
        return lldb.SBExecutionContext(target)
    thread = process.GetSelectedThread()
    if not thread.IsValid():
        return lldb.SBExecutionContext(process)
    frame = thread.GetSelectedFrame()
    if frame.IsValid():
        return lldb.SBExecutionContext(frame)
    return lldb.SBExecutionContext(thread)


def run_lldb_command(debugger: lldb.SBDebugger, command: str) -> typing.Tuple[int, str]:
    """Run an LLDB command, returning its status and its output/error text."""
    return_obj = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(
        command, _get_exe_ctx(debugger), return_obj, False
    )
    # GetStatus() can be non-zero even when the command succeeded.
    status = 0 if return_obj.Succeeded() else return_obj.GetStatus()
    output = return_obj.GetOutput()
    error = return_obj.GetError()
    if output and error:
        return (status, output + "\n" + error)
    return (status, output or error)


hhvm_path: pathlib.Path = pathlib.Path(__file__).parent.parent / "hhvm"
hhvm_types_path: pathlib.Path = (
    pathlib.Path(__file__).parent.parent / "hhvm_types_for_lldb_tests"
)
hhvm_test_path: str = os.environ["HHVM_TEST_DIR"]
scripts_path: str = os.environ["HHVM_LLDB_SCRIPTS"]


class LLDBTestBase(BaseFacebookTestCase, abc.ABC):
    def setUp(self) -> None:
        """Set up a debugger for each test case instance"""
        utils.clear_caches()

        debugger = lldb.SBDebugger.Create()
        debugger.SetAsync(
            False
        )  # Important so that we only stop at our breakpoint, not arbitrary signals
        assert debugger.IsValid(), "Unable to create debugger instance"

        target = debugger.CreateTarget(self.getTargetPath())
        assert target.IsValid(), "Unable to create target"

        if self.launchProcess():
            error = lldb.SBError()
            process = target.Launch(
                debugger.GetListener(),
                self.getArgs(),
                None,
                None,
                None,
                None,
                os.getcwd(),
                0,
                True,
                error,
            )
            assert process.IsValid() and error.Success(), (
                f"Unable to launch process ({get_lldb_object_description(error)})"
            )
            assert process.GetState() == lldb.eStateStopped, (
                "Process is not in Stopped state"
            )

        (status, output) = run_lldb_command(
            debugger, f"command script import {scripts_path}/hhvm.py"
        )
        assert status == 0, output  # output will be the error message

        self.debugger = debugger

    @abc.abstractmethod
    def launchProcess(self) -> bool: ...

    @abc.abstractmethod
    def getTargetPath(self) -> str: ...

    @abc.abstractmethod
    def getArgs(self) -> typing.List[str]: ...

    def tearDown(self) -> None:
        """Clean up the debugger"""
        lldb.SBDebugger.Destroy(self.debugger)
        self.debugger = None

    @property
    def target(self) -> lldb.SBTarget:
        """Get the target associated wih the debugger"""
        return self.debugger.GetSelectedTarget()

    @property
    def process(self) -> lldb.SBProcess:
        """Get the process associated wih the debugger's target"""
        process = self.target.GetProcess()
        assert process.IsValid(), (
            f"Invalid process; self.launchProcess() returns '{self.launchProcess()}'"
        )
        return process

    @property
    def thread(self) -> lldb.SBThread:
        """Get the process associated wih the debugger's target"""
        return self.process.GetSelectedThread()

    @property
    def frame(self) -> lldb.SBFrame:
        """Get the topmost frame associated with the current thread"""
        return self.thread.GetFrameAtIndex(0)

    def run_commands(
        self, commands: typing.List[str], check: bool = True
    ) -> typing.Tuple[int, str]:
        """Run one or more LLDB commands in the interpreter.

        Arguments:
            commands: List of commands (strings) to run
            check: If True (default), check that each command succeeds before running the next

        Returns:
            A (status, output) tuple corresponding to the status of the last command ran
        """
        for command in commands:
            (status, output) = run_lldb_command(self.debugger, command)
            if check:
                self.assertEqual(status, 0, output)
        # pyre-fixme[61]: `status` is undefined, or not always defined.
        # pyre-fixme[61]: `output` is undefined, or not always defined.
        return (status, output)

    def run_until_breakpoint(self, breakpoint: str) -> None:
        """Run until the breakpoint given by a function name is hit"""
        breakpoint = self.debugger.GetSelectedTarget().BreakpointCreateByName(
            breakpoint
        )
        self.assertTrue(
            # pyrefly: ignore [missing-attribute]
            breakpoint.IsValid(),
            f"Unable to set breakpoint at {breakpoint}",
        )
        err = self.process.Continue()
        assert err.Success(), f"Unable to continue to breakpoint {breakpoint}"
        assert (
            self.process.GetSelectedThread().GetStopReason()
            == lldb.eStopReasonBreakpoint
        ), f"Unable to reach breakpoint at {breakpoint}"


class TestHHVMBinary(LLDBTestBase):
    def file(self) -> str | None:
        return None

    def launchProcess(self) -> bool:
        return True

    def getTargetPath(self) -> str:
        return hhvm_path.as_posix()

    def __isHhas(self) -> bool:
        test_file = self.file()
        return test_file is not None and test_file.endswith(".hhas")

    def interp(self) -> bool:
        return self.__isHhas()

    def getArgs(self) -> typing.List[str]:
        hhvm_args = []
        test_file = self.file()
        if test_file:
            hhvm_args.extend(["--file", f"{hhvm_test_path}/{test_file}"])

        if self.interp():
            hhvm_args.extend(["-vEval.Jit=0"])

        if self.__isHhas():
            hhvm_args.extend(["-vEval.AllowHhas=true"])
        return hhvm_args


class TestHHVMTypesBinary(LLDBTestBase):
    @abc.abstractmethod
    def kindOfTest(self) -> str: ...

    def launchProcess(self) -> bool:
        return True

    def getTargetPath(self) -> str:
        return hhvm_types_path.as_posix()

    def getArgs(self) -> typing.List[str]:
        return [self.kindOfTest()]
