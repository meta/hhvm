// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use ir_core::BlockId;
use ir_core::Call;
use ir_core::Func;
use ir_core::FuncBuilder;
use ir_core::Instr;
use ir_core::instr::Terminator;

/// Turn async calls into normal calls by following the eager return edge.
///
/// Purposely leaves the HasAsyncEagerOffset flag set so later code can
/// determine that this was originally an async call.
pub fn unasync(func: &mut Func) {
    // Go through the blocks and rewrite:
    //
    //     call_async(params) to lazy, eager
    // ->
    //     %n = call(params)
    //     jmp_arg eager(%n)
    //
    // And then let cleanup snap everything together.

    let mut changed = false;

    FuncBuilder::borrow_func(func, |builder| {
        for bid in builder.func.repr.block_ids() {
            builder.start_block(bid);

            // Steal the terminator so we can dissect it.
            let term = std::mem::replace(
                builder.func.repr.terminator_mut(bid),
                Terminator::Unreachable,
            );

            match term {
                Terminator::CallAsync(deref!(call), [lazy, eager]) => {
                    // Remove the (now unreachable) terminator from the block.
                    changed = true;
                    builder.cur_block_mut().iids.pop();
                    rewrite_async_call(builder, call, [lazy, eager]);
                }
                term => {
                    *builder.func.repr.terminator_mut(bid) = term;
                }
            }
        }
    });

    if changed {
        crate::clean::run(func);
    }
}

fn rewrite_async_call(builder: &mut FuncBuilder, call: Call, [_lazy, eager]: [BlockId; 2]) {
    let loc = call.loc;
    let vid = builder.emit(Instr::call(call));
    builder.emit(Instr::Terminator(Terminator::JmpArgs(
        eager,
        Box::new([vid]),
        loc,
    )));
}

#[cfg(test)]
mod test {
    use testutils::Block;
    use testutils::build_test_func;

    use super::*;

    #[test]
    fn basic() {
        let mut f1 = build_test_func(&[
            Block::call_async("b0", "bar", ["b1", "b4"]),
            Block::jmp_op("b1", ["b2", "b3"]).with_param("p1"),
            Block::jmp_arg("b2", "b5", "p1"),
            Block::jmp_arg("b3", "b5", "p5").with_named_target("p5"),
            Block::jmp_arg("b4", "b5", "p7").with_param("p7"),
            Block::ret_value("b5", "p9").with_param("p9"),
        ]);

        unasync(&mut f1);

        let f2 = build_test_func(&[Block::ret_value("b0", "p0").with_named_target("p0")]);

        testutils::assert_func_struct_eq(&f1, &f2);
    }
}
