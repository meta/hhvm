# Suites

Tests are grouped into "suites". They are just directories. Suites can have
subdirectories if you want to group them even more. Running a suite will run
all sub-suites.

* quick - High quality high signal tests. No duplicated logic. If you aren't
  sure, your test doesn't belong here.
* slow - Slower full featured tests. Grouped into sub-suites. By default put
  your test here.
* zend/good - Passing tests from Zend's suite.

# Examples how to run them

* Quick suite with the JIT on -
`test/run test/quick`

* Zend tests just with the interpreter in RepoAuthoritative mode -
`test/run test/zend/good -m interp -r`

* Slow tests with the JIT in PGO mode -
`test/run test/slow -m pgo`

* Run everything that is supposed to pass -
`test/run all`

* Run just the slow Hack typechecker tests -
`test/run --typechecker slow`

# File Layout

The format is the same as Zend's `.phpt` but instead of sections it is
separate files with the section name converted to an extension. This allows
you to easily run the `.php` file without first running the test suite.

These are the allowed extensions:

* `.php` - The source of the test.
* `.php.expect` - The exact string expected output.
* `.php.expectf` - The exact string expected output with formatting characters.
* `.php.out` - When you run the test, the output will be stored here.
* `.php.config` - Runtime and compiler options for the test. Holds the `opts` and `hphp_opts` entries; see "The `.php.config` File" below.
* `.php.cli_args` - Command line arguments to the test file (e.g., `$argv` options).
* `.php.serial` - The test will always be put in the serial bucket to be run sequentially with other serial tests so as to avoid any timing problems or collisions.
* `.php.diff or hhas.diff` - The diff for .expect tests.
* `.hhas` - HipHop Assembly.
* `.php.norepo` – If this file is present, then the test is not run in repo-authoritative mode. Repo-authoritative mode performs a whole-program build.
* `.php.noserver` - Don't run the test in server mode.
* `.php.hhconfig` - A blank or syntactically valid Hack typechecker configuration file if you want the test to be able to be run in typechecker mode.
* `inc.php` - Use this extension for `require` or `include` files if you are going to have a typechecker test that uses them. For now, make sure they are in the same directory as the test. They will be copied along with the core test files when the test runner is executing.

You must have one `.php` (or `.php.type-errors` file -- see below); if you are
running tests against HHVM, you must have one and only one of `.php.expect`,
`.php.hhvm.expect`, `.php.expectf`, `.php.hhvm.expectf`, or
`.php.expectregex`; if you are running tests against the  typechecker, you
must have one and only one of `.php.typechecker.expect` or
`.php.typechecker.expectf`, and you must have a `.php.hhconfig` file as well;
and the rest are optional.

NOTE: If you are using a `.php.type-errors` file, then all the files suffixes listed in the paragraph above will include `type-errors` (e.g., `.php.type-errors.hhvm.expectf`).

NOTE: You can have both a `.php.[hhvm].expect[f]` and a
`.php.typechecker.expect[f]`.

## The `.php.config` File

A test's options live in a single `.php.config` file. Each entry is
`key = value`, where the value is either the rest of the line or a block of
following indented lines:

```
; Runtime options, given on a single line.
opts = --count=2 -vEval.EnableArgsInBacktraces=true
; Compiler options, given as an indented block, one per line.
hphp_opts =
  --inputs=__DIR__/helper_a.inc
  --inputs=__DIR__/helper_b.inc
```

Lines whose first non-space character is `;` or `#` are comments.

The recognized keys are:

* `opts` - Runtime options to pass to hhvm.
* `hphp_opts` - Options passed to hphp when generating a bytecode repo.

An entry with an empty value is still meaningful: as with the old sidecar
files, a test that sets `opts` at all is excluded from server mode.

The per-test `.php.opts` and `.php.hphp_opts` sidecar files these entries
replaced are no longer read at all. Leaving one next to a test is an error
rather than a silent no-op, so a stale file gets noticed.

Suite-wide defaults live in a per-directory `config.config` file using the
same format. A test's own `.php.config` overrides the directory default for
whichever keys it sets. The lookup is per-key and walks up the directory tree,
so a nested `config.config` that only sets `opts` still inherits `hphp_opts`
from further up.

Any suite can have a `config.hdf` file in it that will be used. If one isn't
present, then the parent suite it checked recursively until we use
test/config.hdf.

If a suite contains an `hphpd.ini` file, all of the files in the suite will be
run with the `-m debug` and `--debug-config _dir_/hphpd.ini` switches added to
the command line. (`_dir_` will be replaced by path of the suite directory.)

Name your test in a descriptive manner and when in doubt break your test into
many files. You can use comments too so future engineers know if it is a real
breakage or they need to change the expected output.

## `.php.type-errors` Source File Suffix

The test runner will look for source code files with the `.php.type-errors`
suffix as well as `.php` files. The use case for this type of file is create a
test runner compliant source file, but to also allow the actual typechecker
`hh_client` ignore the file when run on its own. For example, in our doc repo
https://github.com/hhvm/user-documentation, we use these files to have a clean
`hh_client` run on our examples, but still can have examples showing
typechecker errors as well.

## Format Characters

These can appear in `.expectf` files.

| Char        | Description
|-------------|----------------------------------------------------------
| %s          | Any characters except newlines                          |
| %S          | Optionally any characters except newlines               |
| %a          | Any characters                                          |
| %A          | Optionally any characters                               |
| %w          | Optional whitespace                                     |
| %i          | Integer with optional sign                              |
| %d          | Digits                                                  |
| %x          | Hex                                                     |
| %f          | Float                                                   |
| %c          | Character                                               |
| %C          | Optional character                                      |
| %t          | Tab                                                     |
| %%          | %                                                       |
| %h{xx}      | Literal byte with the value given inside {} (in hex)    |
| %?{...}     | Optionally the sub pattern inside {}                    |
| %|{...|...} | One of the sub patterns inside {} (separated by |)      |
| %*{...}     | Zero or more of the sub pattern inside {}               |
