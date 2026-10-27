# `hh_single_type_check` analysis and cleanup

## Scope

This analysis covers all 864 `.php` test sources recursively under
`hphp/test/quick`. Paths in the file lists are relative to that directory.

The four `.hhas` sources and helper `.inc` files were excluded because they are
not standalone Hack source inputs. There are no nested `.php.type-errors`
files.

Each file was checked independently using the repository-built
`hh_single_type_check` target in `@//mode/dev-nosan-lg`. There is no `.hhconfig`
or `HH_FLAGS` file governing this directory; `hphp_config.ini` and the test
`.config` files configure HHVM execution rather than the Hack typechecker.

## Summary

| Category | Count | Meaning |
| --- | ---: | --- |
| Passed without edits | 108 | No hard typechecker errors. Six files emit warnings. |
| Mechanically fixed | 179 | Declaration issues were fixed and all files now pass `hh_single_type_check` and their runtime expectations. |
| Substantive failures | 575 | The tests depend on syntax, naming, dynamic-runtime, or type-invalid behavior that cannot be fixed mechanically without changing or suppressing what is being tested. |
| Checker-pathological | 2 | The parser stress tests did not complete after approximately eight minutes. |

After the cleanup, 287 of the 864 `.php` tests pass the typechecker.

The “mechanically fixable” category is based on the current diagnostic surface.
It includes files whose only hard errors have these codes:

- `Naming[2001]`, `Naming[2035]`: missing type hints
- `Naming[2042]`: lowercase type-parameter name
- `Naming[2086]`: missing member visibility
- `Typing[4030]`, `Typing[4032]`, `Typing[4033]`: missing return, parameter,
  property, or variadic type hints
- `Typing[4422]`: a function that returns no value is not declared `void`

This does not mean that adding `dynamic` or suppressions would constitute a
useful typing improvement. Precise annotations should preserve each runtime
test's behavior, and a recheck may reveal follow-on errors.

## HHVM frontend rejection

Running `hhvm -l` independently on all 864 sources accepts 806 files and
rejects the following 58 before execution. This mode includes both grammar
parsing and immediate frontend validation, so failures such as illegal class
structure, incompatible default values, and duplicate declarations are listed
alongside syntax errors.

Of the 258 files in HHSTC's parser-or-NAST group, HHVM accepts 215 and rejects
43. Another 13 files classified elsewhere by HHSTC are rejected by the HHVM
frontend, and the two HHSTC-pathological expression tests fail quickly at
HHVM's expression-recursion limit.

```text
Xhp.php
Xhp2.php
class_abstract_final2.php
class_abstract_final4.php
class_abstract_final5.php
class_constant.php
closure_use_this.php
ctor_param_promotion_abstract.php
ctor_param_promotion_generic.php
ctor_param_promotion_generic1.php
ctor_param_promotion_interface.php
ctor_param_promotion_redeclaration.php
ctor_param_promotion_trait.php
dv_ab.php
dv_af.php
dv_ai.php
dv_as.php
dv_bf.php
dv_bi.php
dv_bs.php
dv_ib.php
dv_if.php
dv_is.php
dv_si.php
dv_si_func.php
foreach-bad.php
function_pointer/bad_syntax.php
function_pointer/class_const_this.php
function_pointer/obj_get_simple.php
hh_bad_start.php
hh_numbers.php
interface-bad-methodbody.php
interface_implements_interface.php
memoize_lsb/memoize_lsb_non_static.php
memoize_lsb/memoize_lsb_on_function.php
method_param_promotion_error.php
ns_existing_names_class.php
on_list_assign.php
parse_fail_shapes.php
parse_fail_shapes2.php
parse_fail_shapes3.php
parse_fail_strict_hashbang_whitespace_start.php
parse_fail_type_constraint.php
parser_massive_add_exp.php
parser_massive_concat_exp.php
post_try_error.php
redeclared_function.php
redeclared_method.php
reserved_class1.php
syntax-error.php
trailing_comma_bad1.php
trailing_comma_bad2.php
trailing_comma_bad3.php
trailing_comma_bad4.php
trailing_comma_bad5.php
trailing_comma_bad6.php
traits2.php
xhp-malformed.php
```

## Passing tests

Of these 108 tests, 102 are silent. The following six pass with warnings:
`apc-keys.php`, `ext.php`, `ext_pdo_fetchobject_no_class_name.php`,
`floateq.php`, `mod.php`, and `openssl_binarysafety.php`.

```text
SetM.php
apc-keys.php
array.php
array_cmp.php
array_cse.php
array_values.php
array_wrap.php
autoload4.php
chdir.php
chdir_posix.php
class_implements.php
closure_get_declared_classes.php
cmp.php
compiler_frontend_constant.php
constants-hni-repo.php
div-one.php
div-power-of-two.php
div-same.php
div_fpe.php
do-while-continue.php
dict/scalar.php
dict/unset.php
empty_stdClass_prop.php
enormous-string.php
ext.php
ext_pdo_exception.php
ext_pdo_fetchobject_no_class_name.php
extclscns.php
fc_enum_8.php
fc_enum_9.php
file_blank.php
file_read.php
file_rw.php
finally_break.php
floateq.php
foreach_concurrent.php
func.php
function_pointer/class_const_self.php
function_pointer/class_const_simple.php
function_pointer/class_const_static.php
function_pointer/class_const_visibility.php
function_pointer/function_pointer_simple.php
function_pointer/vec_map.php
get_class_constants.php
hash_algos.php
headers_sent_cli_echo.php
headers_sent_cli_twice.php
hello.php
hh.php
hoistable_b.php
hopt-sub1.php
hopt-true3.php
hsl_systemlib.php
hsl_systemlib_disabled.php
iconv.php
ini_get_set_memory_limit.php
inout-closure.php
is_dir.php
json_bigint.php
json_escaped_string.php
json_last_error.php
json_last_error_scalar.php
keyset/scalar.php
lambda1.php
lambda3.php
lambda6.php
lc.php
lexer_heredoc_var_newlines.php
libxml_suppress_errors.php
mbstring-oob.php
memoize_lsb/memoize_lsb_clobber.php
merge_globals.php
mod.php
negjump.php
no_more_id_crazy.php
nonstrict_hashbang_start.php
ns_reflection.php
openssl_binarysafety.php
parser_reasonable_add_exp.php
parser_reasonable_concat_exp.php
preposts.php
printf.php
qop2.php
reflection_default_param.php
regalloc.php
rejit.php
reqonce2.php
sb_overflow.php
setelemloc.php
setnewelemloc.php
setnewelemloc2.php
shapes_key_escaping.php
simp-add-dbl.php
simp-conv-to-int-dbl.php
static-arr.php
static_array.php
strict_hashbang_start.php
string-escape.php
string_replace_overflow.php
strrpos.php
test-binary-octal.php
type_cycle.php
unpack.php
unserialize_assert.php
var.php
xml_entity_loader.php
xmlwriter_null_ns_prefix.php
zero_eval_stack_depth.php
```

## Mechanically fixed tests

These 179 files had only the declaration-hygiene errors listed above. They now
pass `hh_single_type_check`, and their existing HHVM runtime expectations also
pass. `memoize_lsb/memoize_lsb_recursive.php` retains two warnings about
intentionally overridden static properties.

Six are mechanically straightforward but are not small edits because they have
more than 20 diagnostics:

| File | Diagnostics |
| --- | ---: |
| `args-test.php` | 365 |
| `giant-class.php` | 100 |
| `constants-arrays.php` | 83 |
| `lotta-args.php` | 78 |
| `class_constant_array.php` | 38 |
| `hopt-mul-simp.php` | 21 |

```text
Cls.php
FPushClsMethodF.php
IncDecL1.php
IncDecL2.php
SetOpM.php
SetThisProp.php
abs_codegen.php
args-test.php
array-compact.php
array_packed.php
async_suspend_stack.php
bitwise.php
boxed_static_string.php
byref.php
castDbl.php
class-call-target.php
class_constant_array.php
class_interface_trait_exists.d/class_interface_trait_exists.php
clsconstant_redecl3.php
clsmethodf.php
cnvStr.php
collection-exceptions.php
concat.php
constIntToString.php
const_cflow.php
const_exprs.php
constants-arrays.php
continuation_inherit.php
continuation_serialize.php
crossUnitRefsInc.php
ctor_param_promotion_genericgood.php
cuf07.php
datetime.php
debugger/break4.php
dec-minint.php
dict/builtins.php
dict/names.php
div-zero-new.php
div-zero.php
doublejump.php
eq_int_str.php
exception_bug_1351681.php
exception_handler.php
fc_enum_7.php
foreach.php
gcv.php
get_class.php
get_class_methods1.php
get_class_methods3.php
get_class_methods4.php
giant-class.php
guards1.php
hack-array-print.php
hh_functions.php
hh_functions2.php
hopt-add-sub.php
hopt-and-simp.php
hopt-arg-alias.php
hopt-arg-alias2.php
hopt-cjump.php
hopt-cmp1.php
hopt-concat-simp.php
hopt-eq.php
hopt-eqBoolInt.php
hopt-juggling.php
hopt-mul-simp.php
hopt-or-simp.php
hopt-pre-coloring-2.php
hopt-pre-coloring.php
hopt-ref-arg.php
hopt-ref-medium.php
hopt-ref-obj.php
hopt-ref-simple.php
hopt-refs1.php
hopt-reg-spill.php
hopt-same-simp.php
hopt-simp-rare.php
hopt-static.php
hopt-stref2.php
hopt-sub-simp.php
hopt-true4.php
hopt-type-mutate1.php
hopt-type-mutate2.php
hopt-type-mutate3.php
hopt-xor-simp.php
hopt_cgetm.php
hopt_late_jcc_to_jmp.php
hopt_static_call.php
incdec-1.php
incdec-2.php
indirect-fn.php
inout-function.php
inout-method.php
intercept2.php
interface2.php
isset_hackarr.php
jmpfuse.php
json_depth.php
keyset/builtins.php
keyset/names.php
keyset/serialize.php
lexer_heredoc_buffer_boundary_65492.php
lexer_heredoc_buffer_boundary_65493.php
lexer_heredoc_buffer_boundary_65494.php
lexer_heredoc_label_cornercases.php
lexer_nowdoc_buffer_boundary_65492.php
lexer_nowdoc_buffer_boundary_65493.php
lexer_nowdoc_buffer_boundary_65494.php
lexer_nowdoc_label_cornercases.php
lotta-args.php
lsb_static_init_deep.php
lsb_static_init_simple.php
many-clsrefs.php
memoize_lsb/memoize_lsb_mixup.php
memoize_lsb/memoize_lsb_no_args.php
memoize_lsb/memoize_lsb_recursive.php
method-undefined.php
mod-zero-new.php
normalize.php
nonnull/reflect.php
nonnull/type-hint.php
nothing/reflect.php
nothing/type-hint.php
ns_existing_names.php
null/reflect.php
null/type-hint.php
null_constant.php
packed-array-bounds.php
packed-isset.php
parse_type_not_typedef.php
parser_reasonable_nested_array.php
profile/Setprofile_closure.php
profile/setprofile_throw.php
prop_ctx.php
propagate-array.php
property_exists.php
raise_array_index_notice.php
redundant-ldref1.php
reflection_is_async.php
reflection_quick.php
regalloc-fun.php
regpressure.php
reified-and-variadic.php
rename_dyn_function.php
rename_function_disable.php
return-from-foreach.php
retype.php
rm-packed.php
rounding.php
selfparent.php
setmstr.php
setmstr2.php
setop_div_zero_dce.php
setopl.php
short_array_syntax.php
shuffle1.php
shuffle2.php
simp-mul-dbl.php
simp-sub-dbl.php
simpl-0.php
simpl-1.php
some_cmp_tests.php
stack_overflow_variadic_capture.php
strToInt.php
string-cse.php
string-util.php
string_copy.php
string_replace.php
switch_special_cases.php
syntax-error-incl.php
trailing_comma.php
trait_method_modif10.php
trait_self.php
typehint_arraykey.php
undefined-global.php
vec/builtins.php
vec/names.php
xmm-spill1.php
yermo.php
```

## Substantive or essentially untypeable tests

The remaining 575 `.php` files have at least one non-mechanical diagnostic.
They divide into these disjoint groups, assigning each file to its earliest
applicable group:

| Failure group | Count | Typical examples |
| --- | ---: | --- |
| Parser or NAST rejection | 258 | Deliberately invalid syntax, illegal class structures, direct constructor calls, invalid `break`/`continue`, or other constructs rejected before ordinary typing. |
| Naming or dynamic-resolution failure | 98 | Undefined variables, classes, functions, or constants; duplicate declarations; dynamic method/property access; and runtime-generated names. |
| Semantic typing failure | 219 | Wrong argument types or counts, nullable access, invalid conditions, visibility violations, illegal casts/comparisons, invalid overrides, and operations intentionally performed on incompatible values. |

These tests are not necessarily impossible to make syntactically accepted.
They could generally be silenced with `HH_FIXME`, `dynamic`, unsafe casts, or a
rewrite, but doing so would either discard useful type information or change
the runtime behavior being tested. They therefore are not good candidates for
a routine type-annotation cleanup.

The substantive category is exactly every `.php` file not listed in the
passing or mechanically fixed sections, except for the two stress tests below.

## Checker-pathological tests

```text
parser_massive_add_exp.php
parser_massive_concat_exp.php
```

Both deliberately contain enormous expression trees. Separate HHST processes
for them remained CPU-bound and produced no result after approximately eight
minutes. Their smaller `parser_reasonable_*` counterparts both pass.
