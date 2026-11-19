# Copyright 2022-present Facebook. All Rights Reserved.

# pyre-strict

from . import base  # usort: skip (must be first, needed for sys.path side-effects)


class PrettyPrintTypedValuesTestCase(base.TestHHVMTypesBinary):
    def kindOfTest(self) -> str:
        return "typed-values"

    def test_pp_tvs(self) -> None:
        # Printing all of them in one execution for efficiency.
        # Thus, the order here matters (i.e. use the order in the DataType enum and
        # order of other calls in the test binary).

        with self.subTest("TypedValue (Vec)"):
            self.run_until_breakpoint("takeTypedValueVec")
            _, output = self.run_commands(["frame variable tv"])
            expected_lines = [
                "(HPHP::TypedValue) tv = { Vec, (HPHP::ArrayData) *parr = 3 element(s) {",
                "[0] = { Int64, 1 }",
                "[1] = { Int64, 2 }",
                "[2] = { Int64, 3 }",
                "} }",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        breakpoints_to_outputs = {
            "String": r'\(HPHP::TypedValue\) tv = \{ String, "Hello, world!" \}',
            "Object": r'\(HPHP::TypedValue\) tv = \{ Object, \(HPHP::ObjectData \*\) pobj = 0x.* "InvalidArgumentException" \}',
            "Resource": r"\(HPHP::TypedValue\) tv = \{ Resource, \(hdr = 0x.*, data = 0x.*\) *\}",
            "RFunc": r'\(HPHP::TypedValue\) tv = \{ RFunc, \(HPHP::RFuncData \*\) prfunc = 0x.* \("Exception::__construct"\) \}',
            "RClsMeth": r'\(HPHP::TypedValue\) tv = \{ RClsMeth, \(HPHP::RClsMethData \*\) prclsmeth = 0x.* \("InvalidArgumentException::Exception::__construct"\) \}',
            "ClsMeth": r'\(HPHP::TypedValue\) tv = \{ ClsMeth, \(HPHP::ClsMethData::cls_meth_t\) m_data = \(m_cls = "InvalidArgumentException", m_func = "Exception::__construct"\) \("InvalidArgumentException::Exception::__construct"\) }',
            "Boolean": r"\(HPHP::TypedValue\) tv = \{ Boolean, True \}",
            "Int64": r"\(HPHP::TypedValue\) tv = \{ Int64, 42 \}",
            "Double": r"\(HPHP::TypedValue\) tv = \{ Double, 3.1415 \}",
            "Func": r'\(HPHP::TypedValue\) tv = \{ Func, \(const HPHP::Func \*\) pfunc = 0x.* "Exception::__construct".* \}',
            "Class": r'\(HPHP::TypedValue\) tv = \{ Class, \(HPHP::Class \*\) pclass = 0x.* "InvalidArgumentException" \}',
            "LazyClass": r'\(HPHP::TypedValue\) tv = \{ LazyClass, \(HPHP::LazyClassData\) plazyclass = "SpecialLazyClass" \}',
            "Uninit": r"\(HPHP::TypedValue\) tv = \{ Uninit \}",
            "Null": r"\(HPHP::TypedValue\) tv = \{ Null \}",
        }

        for tv_name, expected_output in breakpoints_to_outputs.items():
            with self.subTest(f"TypedValue ({tv_name})"):
                self.run_until_breakpoint(f"takeTypedValue{tv_name}")
                _, output = self.run_commands(["frame variable tv"])
                self.assertRegex(output.strip(), expected_output)

        with self.subTest("TypedValue (reference)"):
            self.run_until_breakpoint("takeTypedValueRef")
            _, output = self.run_commands(["frame variable tv"])
            # LLDB pretty prints it, and then for some strange reason proceeds to
            # print out the raw structure contents, hence the extra .*
            expected_output = r"\(HPHP::TypedValue &\) tv = 0x.* \{ Int64, 42 \}.*"
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("TypedValue (pointer)"):
            # Testing that LLDB prints the address of the pointer before
            # automatically pretty printing the dereferenced value
            self.run_until_breakpoint("takeTypedValuePtr")
            _, output = self.run_commands(["frame variable tv"])
            expected_output = r"\(HPHP::TypedValue \*\) tv = 0x.* \{ Int64, 42 \}"
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("TypedValue (subclasses -- Variant)"):
            self.run_until_breakpoint("takeVariant")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r"\(HPHP::Variant\) v = \{ Int64, 42 \}"
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("TypedValue (subclasses -- VarNR)"):
            self.run_until_breakpoint("takeVarNR")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r"\(HPHP::VarNR\) v = \{ Double, 2.718 \}"
            self.assertRegex(output.strip(), expected_output)

    def test_pp_tvs_2(self) -> None:
        with self.subTest("TypedValue (Dict)"):
            self.run_until_breakpoint("takeTypedValueDict")
            _, output = self.run_commands(["frame variable tv"])
            expected_lines = [
                "(HPHP::TypedValue) tv = { Dict, (HPHP::ArrayData) *parr = 3 element(s) {",
                '"key1" = { Int64, 1 }',
                '"key2" = { Double, 2.718 }',
                '"key3" = { String, "Hello, world!" }',
                "} }",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        with self.subTest("TypedValue (Keyset)"):
            self.run_until_breakpoint("takeTypedValueKeyset")
            _, output = self.run_commands(["frame variable tv"])
            expected_lines = [
                "(HPHP::TypedValue) tv = { Keyset, (HPHP::ArrayData) *parr = 3 element(s) {",
                "= { Int64, 1 }",
                "= { Int64, 2 }",
                "= { Int64, 3 }",
                "} }",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)


class PrettyPrintOtherValuesTestCase(base.TestHHVMTypesBinary):
    def kindOfTest(self) -> str:
        return "other-values"

    def test_pp_other_values(self) -> None:
        with self.subTest("StringData"):
            self.run_until_breakpoint("takeStringData")

            _, output = self.run_commands(["frame variable *v"])
            expected_output = '(HPHP::StringData) *v = "hello"'
            self.assertEqual(output.strip(), expected_output)

            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(HPHP::StringData \*\) v = 0x.* "hello"'
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("StringData * const"):
            self.run_until_breakpoint("takeConstPtrToStringData")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(HPHP::StringData \*const\) v = 0x.* "hello"'
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("const StringData *"):
            self.run_until_breakpoint("takePtrToConstStringData")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(const HPHP::StringData \*\) v = 0x.* "hello"'
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("String"):
            # main.cpp's takeString now takes an HPHP::OptString (the base of
            # HPHP::String), so the debugger reports the value's type as OptString.
            self.run_until_breakpoint("takeString")
            _, output = self.run_commands(["frame variable v"])
            expected_output = '(HPHP::OptString) v = "hello"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("String *"):
            self.run_until_breakpoint("takePtrToString")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(HPHP::OptString \*\) v = 0x.* "hello"'
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("StaticString"):
            self.run_until_breakpoint("takeStaticString")
            _, output = self.run_commands(["frame variable v"])
            expected_output = '(HPHP::StaticString) v = "hello"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("StrNR"):
            self.run_until_breakpoint("takeStrNR")
            _, output = self.run_commands(["frame variable v"])
            expected_output = '(HPHP::StrNR) v = "hello"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("OptResource"):
            self.run_until_breakpoint("takeResource")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r"\(HPHP::OptResource\) v = \(hdr = 0x.*, data = 0x.*\)"
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("OptResource *"):
            self.run_until_breakpoint("takePtrToResource")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                r"\(HPHP::OptResource \*\) v = 0x.* \(hdr = 0x.*, data = 0x.*\)"
            )
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("Object"):
            self.run_until_breakpoint("takeObject")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'(HPHP::Object) v = "InvalidArgumentException"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("req::ptr"):
            self.run_until_breakpoint("takeReqPtr")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                '(HPHP::req::ptr<HPHP::ObjectData>) v = "InvalidArgumentException"'
            )
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::Optional (Some)"):
            self.run_until_breakpoint("takeOptional")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                "(HPHP::Optional<HPHP::OptString>) v = "
                '(HPHP::OptString) Value = "hello"'
            )
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::Optional (None)"):
            self.run_until_breakpoint("takeOptional")
            _, output = self.run_commands(["frame variable v"])
            expected_output = "(HPHP::Optional<HPHP::OptString>) v = None"
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::PackedPtr"):
            # clang 21 with enable_simple_template_names=False emits the
            # fully-qualified template argument HPHP::Class (older toolchains
            # emitted the unqualified Class).
            self.run_until_breakpoint("takePackedPtr")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                '(HPHP::PackedPtr<HPHP::Class>) v = "InvalidArgumentException"'
            )
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::PackedPtr &"):
            self.run_until_breakpoint("takePackedPtrRef")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(const HPHP::PackedPtr<HPHP::Class> &\) v = 0x.* "InvalidArgumentException"'
            # LLDB always prints the raw contents of references, so just check the first line
            self.assertRegex(output.split("\n")[0].strip(), expected_output)

        with self.subTest("HPHP::PackedStringPtr"):
            # This is used e.g. as an alias in types.h, so let's make sure
            # we can handle aliases too.
            self.run_until_breakpoint("takePackedStringPtr")
            _, output = self.run_commands(["frame variable v"])
            expected_output = '(HPHP::PackedStringPtr) v = "hello"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::Extension"):
            self.run_until_breakpoint("takeExtension")
            _, output = self.run_commands(["frame variable v"])
            expected_output = "(HPHP::Extension) v = test-extension (version: 0.5, oncall: test-oncall)"
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::ArrayData"):
            self.run_until_breakpoint("takeArrayData")

            _, output = self.run_commands(["frame variable v"])
            expected_output = r"\(HPHP::ArrayData \*\) v = 0x.* 4 element\(s\)"
            self.assertRegex(output.strip(), expected_output)

            _, output = self.run_commands(["frame variable *v"])
            expected_lines = [
                "(HPHP::ArrayData) *v = 4 element(s) {",
                "[0] = { Int64, 1 }",
                "[1] = { Int64, 2 }",
                "[2] = { Int64, 3 }",
                "[3] = { Int64, 4 }",
                "}",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        with self.subTest("HPHP::Array (Vec)"):
            self.run_until_breakpoint("takeArrayVec")
            _, output = self.run_commands(["frame variable v"])
            expected_lines = [
                "(HPHP::Array) v = 4 element(s) {",
                "[0] = { Int64, 1 }",
                "[1] = { Int64, 2 }",
                "[2] = { Int64, 3 }",
                "[3] = { Int64, 4 }",
                "}",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        with self.subTest("HPHP::Array (Dict)"):
            self.run_until_breakpoint("takeArrayDict")
            _, output = self.run_commands(["frame variable v"])
            expected_lines = [
                "(HPHP::Array) v = 5 element(s) {",
                "19122942 = { Boolean, True }",
                '302 = { String, "Salutations, earth!" }',
                "2 = { Double, 3.14 }",
                '"key4" = { Double, 2.718 }',
                '"key5" = { String, "Hello, world!" }',
                "}",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        with self.subTest("HPHP::Array (Keyset)"):
            self.run_until_breakpoint("takeArrayKeyset")
            _, output = self.run_commands(["frame variable v"])
            expected_lines = [
                "(HPHP::Array) v = 6 element(s) {",
                "= { Int64, 1 }",
                '= { String, "cats" }',
                "= { Int64, 2 }",
                "= { Int64, 3 }",
                '= { String, "dogs" }',
                "= { Int64, 42 }",
                "}",
            ]
            actual_lines = [line.strip() for line in output.split("\n") if line]
            self.assertEqual(actual_lines, expected_lines)

        with self.subTest("HPHP::Func"):
            self.run_until_breakpoint("takeFunc")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                r'\(const HPHP::Func \*\) v = 0x.* "Exception::__construct"'
            )
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("HPHP::Class"):
            self.run_until_breakpoint("takeClass")
            _, output = self.run_commands(["frame variable v"])
            expected_output = r'\(HPHP::Class \*\) v = 0x.* "InvalidArgumentException"'
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("HPHP::LazyClassData"):
            self.run_until_breakpoint("takeLazyClassData")
            _, output = self.run_commands(["frame variable v"])
            expected_output = '(HPHP::LazyClassData) v = "SpecialLazyClass"'
            self.assertEqual(output.strip(), expected_output)

        with self.subTest("HPHP::ObjectData"):
            self.run_until_breakpoint("takeObjectData")
            _, output = self.run_commands(["frame variable v"])
            expected_output = (
                r'\(HPHP::ObjectData \*\) v = 0x.* "InvalidArgumentException"'
            )
            self.assertRegex(output.strip(), expected_output)

        with self.subTest("HPHP::Op"):
            for op in ["Nop", "Int", "CGetL", "NewObjD", "QueryM"]:
                self.run_until_breakpoint("takeHhbcOp")
                _, output = self.run_commands(["frame variable v"])
                self.assertEqual(output.strip(), f"(HPHP::Op) v = {op}")

        with self.subTest("HPHP::HHBBC::Bytecode"):
            self.run_until_breakpoint("takeHhbbcBytecode")
            _, output = self.run_commands(["frame variable v"])
            self.assertEqual(
                output.strip(), "(HPHP::HHBBC::Bytecode) v = bc::Nop { Nop = {} }"
            )

            self.run_until_breakpoint("takeHhbbcBytecode")
            _, output = self.run_commands(["frame variable v"])
            self.assertEqual(
                output.strip(),
                "(HPHP::HHBBC::Bytecode) v = bc::Int { Int = (arg1 = 42) }",
            )

            self.run_until_breakpoint("takeHhbbcBytecode")
            _, output = self.run_commands(["frame variable v"])
            self.assertEqual(
                output.strip(),
                "(HPHP::HHBBC::Bytecode) v = bc::CGetL { CGetL = {\n  nloc1 = (name = 1, id = 2)\n} }",
            )
