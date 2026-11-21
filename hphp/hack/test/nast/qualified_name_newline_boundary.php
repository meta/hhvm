<?hh

namespace N;

function my_function(): void {}

function f(): void {
  my_function();
  \some_root_ns_function();

  my_function
  \some_root_ns_function();
}
