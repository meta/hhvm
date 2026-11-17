# Program Structure

A Hack program consists of one or more source files; these source files should
have the extension `.hack`, and contain one or more declarations of the following kind,
in any order:

* alias declaration
* class
* enumerated type
* function
* inclusion directive
* interface
* namespace
* trait
* use

Consider the following file:

```hack
#!/usr/bin/env hhvm

<<__EntryPoint>>
function main(): void {
  print("Hello, World!\n");
}
```

In this example, the start-up function is called `main`; however, that was an arbitrary choice of name; it could just as easily
have been `run`, `do_it`, or `make_magic`. (What makes `main` the start-up function is the presence of the [attribute `__EntryPoint`](/hack/attributes/predefined-attributes#__entrypoint).)

## Including Other Files

A file can import another file via [inclusion](/hack/source-code-fundamentals/script-inclusion).

## Legacy Files

<FbInfo>

All Hack source code at Meta should use the `.php` file extension and Hack strict mode (plain `<?hh` header line).

</FbInfo>

The `.php` extension is still in use in older Hack code, though not recommended for new code. Hack code using
this extension *must* start with an optional shebang line (e.g. `#!/usr/bin/env hhvm`), followed by a header line. The header line can be one of the following:

- `<?hh`: this is the recommended header; strict mode is enabled by default.
- `<?hh // partial`: this loosens several restrictions to ease migration from other languages. It is strongly
  discouraged for new code.

The following file is equivalent to the earlier example:

```hack no-extract
#!/usr/bin/env hhvm
<?hh

<<__EntryPoint>>
function main(): void {
  print("Hello, World!\n");
  exit(0);
}
```
