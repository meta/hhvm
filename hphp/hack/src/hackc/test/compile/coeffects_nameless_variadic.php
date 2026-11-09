<?hh
// RUN: %hackc compile -v Hack.Lang.AllowUnstableFeatures=true %s | FileCheck %s

<<file:__EnableUnstableFeatures('named_parameters')>>

// The nameless named-variadic is not emitted, so coeffect rules must not count
// it when computing runtime parameter indexes.

// CHECK: .function {} [variadic_named_param] ({{[0-9]+}},{{[0-9]+}}) <"HH\\void" N > f(<"(function ()[_]: HH\\void)" N > $cb) {
// CHECK:   .coeffects_fun_param 0;
// CHECK: .function {} [variadic_named_param] ({{[0-9]+}},{{[0-9]+}}) <"HH\\void" N > g(<"(function ()[_]: HH\\void)" N > $cb) {
// CHECK:   .coeffects_cc_param 0 C;

function f((function()[_]: void) $cb, named int...)[ctx $cb]: void {
  $cb();
}

function g((function()[_]: void) $cb, named int...)[$cb::C]: void {
  $cb();
}
