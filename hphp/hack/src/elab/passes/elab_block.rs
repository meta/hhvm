// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use std::collections::VecDeque;

use nast::Block;
use nast::FinallyBlock;
use nast::Stmt;
use nast::Stmt_;
use nast::UsingStmt;

use crate::prelude::*;

#[derive(Clone, Copy, Default)]
pub struct ElabBlockPass;

impl Pass for ElabBlockPass {
    fn on_ty_block_top_down(&mut self, _: &Env, elem: &mut Block) -> ControlFlow<()> {
        let mut q: VecDeque<_> = elem.drain(0..).collect();
        while let Some(Stmt(pos, stmt_)) = q.pop_front() {
            match stmt_ {
                Stmt_::Block(deref!((_, xs))) => xs.into_iter().rev().for_each(|x| q.push_front(x)),
                _ => elem.push(Stmt(pos, stmt_)),
            }
        }
        Continue(())
    }

    fn on_ty_finally_block_top_down(
        &mut self,
        _: &Env,
        elem: &mut FinallyBlock,
    ) -> ControlFlow<()> {
        let mut q: VecDeque<_> = elem.drain(0..).collect();
        while let Some(Stmt(pos, stmt_)) = q.pop_front() {
            match stmt_ {
                Stmt_::Block(deref!((_, xs))) => xs.into_iter().rev().for_each(|x| q.push_front(x)),
                _ => elem.push(Stmt(pos, stmt_)),
            }
        }
        Continue(())
    }
    fn on_ty_using_stmt_top_down(&mut self, _: &Env, elem: &mut UsingStmt) -> ControlFlow<()> {
        elem.is_block_scoped = false;
        Continue(())
    }
}

#[cfg(test)]
mod tests {

    use nast::Block;
    use nast::Pos;
    use nast::Stmt;
    use nast::Stmt_;

    use super::*;

    #[test]
    fn test() {
        let env = Env::default();

        let mut pass = ElabBlockPass;

        let mut elem: Block = Block(vec![Stmt(
            Pos::NONE,
            Stmt_::Block(Box::new((
                None,
                Block(vec![
                    Stmt(Pos::NONE, Stmt_::Noop),
                    Stmt(
                        Pos::NONE,
                        Stmt_::Block(Box::new((
                            None,
                            Block(vec![
                                Stmt(Pos::NONE, Stmt_::Noop),
                                Stmt(
                                    Pos::NONE,
                                    Stmt_::Block(Box::new((
                                        None,
                                        Block(vec![
                                            Stmt(Pos::NONE, Stmt_::Noop),
                                            Stmt(
                                                Pos::NONE,
                                                Stmt_::Block(Box::new((
                                                    None,
                                                    Block(vec![
                                                        Stmt(Pos::NONE, Stmt_::Noop),
                                                        Stmt(
                                                            Pos::NONE,
                                                            Stmt_::Block(Box::new((
                                                                None,
                                                                Block(vec![Stmt(
                                                                    Pos::NONE,
                                                                    Stmt_::Noop,
                                                                )]),
                                                            ))),
                                                        ),
                                                        Stmt(Pos::NONE, Stmt_::Noop),
                                                    ]),
                                                ))),
                                            ),
                                            Stmt(Pos::NONE, Stmt_::Noop),
                                        ]),
                                    ))),
                                ),
                                Stmt(Pos::NONE, Stmt_::Noop),
                            ]),
                        ))),
                    ),
                    Stmt(Pos::NONE, Stmt_::Noop),
                ]),
            ))),
        )]);
        elem.transform(&env, &mut pass);

        assert_eq!(elem.len(), 9);
        assert!(elem.into_iter().all(|s| matches!(s.1, Stmt_::Noop)));
    }
}
