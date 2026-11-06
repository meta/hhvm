/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! Working with the watchman expression term syntax
use std::path::PathBuf;

use maplit::hashmap;
use serde::Serialize;
use serde_bser::value::Value;

use crate::pdu::*;

/// An expression term used to filter candidate files from query results.
#[derive(Serialize, Debug, Clone)]
#[serde(into = "Value")]
pub enum Expr {
    /// Always evaluates to true
    True,

    /// Always evaluates to false
    False,

    /// Inverts the match state of the child term
    Not(Box<Expr>),

    /// Evaluates to true IFF all child terms evaluate true
    All(Vec<Expr>),

    /// Evaluates to true if any child terms evaluate true
    Any(Vec<Expr>),

    /// Match on the parent directory structure
    /// <https://meta.github.io/watchman/docs/expr/dirname.html>
    DirName(DirNameTerm),

    /// Evaluates as true if the file exists, has size 0 and is a regular
    /// file or directory.
    /// <https://meta.github.io/watchman/docs/expr/empty.html>
    Empty,

    /// Evaluates as true if the file exists; this is useful for filtering
    /// out notifications for files that have been deleted.
    /// Note that this term doesn't add value for `path` and `glob` generators
    /// which implicitly add this constraint.
    /// <https://meta.github.io/watchman/docs/expr/exists.html>
    Exists,

    /// Performs a glob-style match against the file name
    /// <https://meta.github.io/watchman/docs/expr/match.html>
    Match(MatchTerm),

    /// Performs an exact match against the file name.
    /// <https://meta.github.io/watchman/docs/expr/name.html>
    Name(NameTerm),

    /// Use PCRE to match the filename.
    /// Note that this is an optional server feature and using this term
    /// on a server that doesn't support this feature will generate an
    /// error in response to the query.
    /// <https://meta.github.io/watchman/docs/expr/pcre.html>
    Pcre(PcreTerm),

    /// Evaluates as true if the specified time property of the file is
    /// greater than the since value.
    /// <https://meta.github.io/watchman/docs/expr/since.html>
    Since(SinceTerm),

    /// Evaluate as true if the size of a file matches the specified constraint.
    /// Files that do not presently exist will evaluate as false.
    /// <https://meta.github.io/watchman/docs/expr/size.html>
    Size(RelOp),

    /// Evaluate as true if the filename suffix (also known as extension)
    /// matches the provided set of suffixes.
    /// Suffix matches are always case insensitive.
    /// `php` matches `foo.php` and `foo.PHP` but not `foophp`.
    /// <https://meta.github.io/watchman/docs/expr/suffix.html>
    Suffix(Vec<PathBuf>),

    /// Evaluate as true if the file type exactly matches the specified type.
    FileType(FileType),
}

impl From<Expr> for Value {
    fn from(val: Expr) -> Self {
        match val {
            Expr::True => "true".into(),
            Expr::False => "false".into(),
            Expr::Not(expr) => Value::Array(vec!["not".into(), (*expr).into()]),
            Expr::All(expr) => {
                let mut expr: Vec<Value> = expr.into_iter().map(Into::into).collect();
                expr.insert(0, "allof".into());
                Value::Array(expr)
            }
            Expr::Any(expr) => {
                let mut expr: Vec<Value> = expr.into_iter().map(Into::into).collect();
                expr.insert(0, "anyof".into());
                Value::Array(expr)
            }
            Expr::DirName(term) => {
                let mut expr: Vec<Value> = vec!["dirname".into(), term.path.try_into().unwrap()];
                if let Some(depth) = term.depth {
                    expr.push(depth.into_term("depth"));
                }
                expr.into()
            }
            Expr::Empty => "empty".into(),
            Expr::Exists => "exists".into(),
            Expr::Match(term) => vec![
                "match".into(),
                term.glob.into(),
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                }
                .into(),
                Value::Object(hashmap! {
                    "includedotfiles".to_string() => term.include_dot_files.into(),
                    "noescape".to_string() => term.no_escape.into()
                }),
            ]
            .into(),
            Expr::Name(term) => vec![
                "name".into(),
                Value::Array(
                    term.paths
                        .into_iter()
                        .map(|p| p.try_into().unwrap())
                        .collect(),
                ),
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                }
                .into(),
            ]
            .into(),
            Expr::Pcre(term) => vec![
                "pcre".into(),
                term.pattern.into(),
                if term.wholename {
                    "wholename"
                } else {
                    "basename"
                }
                .into(),
            ]
            .into(),
            Expr::Since(term) => match term {
                SinceTerm::ObservedClock(c) => {
                    vec!["since".into(), c.into(), "oclock".into()].into()
                }
                SinceTerm::CreatedClock(c) => {
                    vec!["since".into(), c.into(), "cclock".into()].into()
                }
                SinceTerm::MTime(c) => {
                    vec!["since".into(), c.to_string().into(), "mtime".into()].into()
                }
                SinceTerm::CTime(c) => {
                    vec!["since".into(), c.to_string().into(), "ctime".into()].into()
                }
            },
            Expr::Size(term) => term.into_term("size"),
            Expr::Suffix(term) => vec![
                "suffix".into(),
                Value::Array(term.into_iter().map(|p| p.try_into().unwrap()).collect()),
            ]
            .into(),
            Expr::FileType(term) => vec!["type".into(), term.to_string().into()].into(),
        }
    }
}

/// Performs an exact match against the file name.
/// <https://meta.github.io/watchman/docs/expr/name.html>
#[derive(Clone, Debug)]
pub struct NameTerm {
    pub paths: Vec<PathBuf>,
    /// By default, the name is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
}

/// Match on the parent directory structure
/// <https://meta.github.io/watchman/docs/expr/dirname.html>
#[derive(Clone, Debug)]
pub struct DirNameTerm {
    /// The path to a directory
    pub path: PathBuf,
    /// Specifies the matching depth.  A file has depth == 0
    /// if it is contained directory within `path`, depth == 1 if
    /// it is in a direct child directory of `path`, depth == 2 if
    /// in a grand-child directory and so on.
    /// If None, the default is considered to GreaterOrEqual depth 0.
    pub depth: Option<RelOp>,
}

/// Use PCRE to match the filename.
/// Note that this is an optional server feature and using this term
/// on a server that doesn't support this feature will generate an
/// error in response to the query.
/// <https://meta.github.io/watchman/docs/expr/pcre.html>
#[derive(Clone, Debug, Default)]
pub struct PcreTerm {
    /// The perl compatible regular expression
    pub pattern: String,

    /// By default, the name is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
}

/// Encodes the match expression term
/// <https://meta.github.io/watchman/docs/expr/match.html>
#[derive(Clone, Debug, Default)]
pub struct MatchTerm {
    /// The glob expression to evaluate
    pub glob: String,
    /// By default, the glob is evaluated against the basename portion
    /// of the filename.  Set wholename=true to have it match against
    /// the path relative to the root of the project.
    pub wholename: bool,
    /// By default, paths whose names start with a `.` are not matched.
    /// Set include_dot_files=true to include them
    pub include_dot_files: bool,
    /// By default, backslashes in the pattern escape the next character.
    /// To have `\` treated literally, set no_escape=true.
    pub no_escape: bool,
}

/// Specifies a relational comparison with an integer value
#[derive(Clone, Debug)]
pub enum RelOp {
    Equal(usize),
    NotEqual(usize),
    Greater(usize),
    GreaterOrEqual(usize),
    Less(usize),
    LessOrEqual(usize),
}

impl RelOp {
    fn into_term(self, field: &str) -> Value {
        let (op, value) = match self {
            Self::Equal(value) => ("eq", value),
            Self::NotEqual(value) => ("ne", value),
            Self::Greater(value) => ("gt", value),
            Self::GreaterOrEqual(value) => ("ge", value),
            Self::Less(value) => ("lt", value),
            Self::LessOrEqual(value) => ("le", value),
        };
        Value::Array(vec![field.into(), op.into(), value.try_into().unwrap()])
    }
}

/// Evaluates as true if the specified time property of the file is greater
/// than the since value.
/// <https://meta.github.io/watchman/docs/expr/since.html>
#[derive(Clone, Debug)]
pub enum SinceTerm {
    /// Yield true if the file was observed to be modified more recently than
    /// the specified clockspec
    ObservedClock(ClockSpec),

    /// Yield true if the file changed from !exists -> exists more recently
    /// than the specified clockspec
    CreatedClock(ClockSpec),

    /// Yield true if the mtime stat field is >= the provided timestamp.
    /// Note that this is >= because it has 1-second granularity.
    MTime(i64),

    /// Yield true if the ctime stat field is >= the provided timestamp.
    /// Note that this is >= because it has 1-second granularity.
    CTime(i64),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn val(expr: Expr) -> Value {
        expr.into()
    }

    #[test]
    fn exprs() {
        assert_eq!(val(Expr::True), "true".into());
        assert_eq!(val(Expr::False), "false".into());
        assert_eq!(val(Expr::Empty), "empty".into());
        assert_eq!(val(Expr::Exists), "exists".into());
        assert_eq!(
            val(Expr::Not(Box::new(Expr::False))),
            vec!["not".into(), "false".into()].into()
        );
        assert_eq!(
            val(Expr::All(vec![Expr::True, Expr::False])),
            vec!["allof".into(), "true".into(), "false".into()].into()
        );
        assert_eq!(
            val(Expr::Any(vec![Expr::True, Expr::False])),
            vec!["anyof".into(), "true".into(), "false".into()].into()
        );

        assert_eq!(
            val(Expr::DirName(DirNameTerm {
                path: "foo".into(),
                depth: None,
            })),
            vec!["dirname".into(), Value::ByteString("foo".into())].into()
        );
        assert_eq!(
            val(Expr::DirName(DirNameTerm {
                path: "foo".into(),
                depth: Some(RelOp::GreaterOrEqual(1)),
            })),
            vec![
                "dirname".into(),
                Value::ByteString("foo".into()),
                vec!["depth".into(), "ge".into(), 1.into()].into()
            ]
            .into()
        );

        assert_eq!(
            val(Expr::Match(MatchTerm {
                glob: "*.txt".into(),
                ..Default::default()
            })),
            vec![
                "match".into(),
                "*.txt".into(),
                "basename".into(),
                hashmap! {
                    "includedotfiles".to_string() => Value::Bool(false),
                    "noescape".to_string() => Value::Bool(false),
                }
                .into()
            ]
            .into()
        );

        assert_eq!(
            val(Expr::Match(MatchTerm {
                glob: "*.txt".into(),
                wholename: true,
                include_dot_files: true,
                ..Default::default()
            })),
            vec![
                "match".into(),
                "*.txt".into(),
                "wholename".into(),
                hashmap! {
                    "includedotfiles".to_string() => Value::Bool(true),
                    "noescape".to_string() => Value::Bool(false),
                }
                .into()
            ]
            .into()
        );

        assert_eq!(
            val(Expr::Name(NameTerm {
                paths: vec!["foo".into()],
                wholename: true,
            })),
            vec![
                "name".into(),
                vec![Value::ByteString("foo".into())].into(),
                "wholename".into()
            ]
            .into()
        );

        assert_eq!(
            val(Expr::Pcre(PcreTerm {
                pattern: "foo$".into(),
                wholename: true,
            })),
            vec!["pcre".into(), "foo$".into(), "wholename".into()].into()
        );

        assert_eq!(
            val(Expr::FileType(FileType::Regular)),
            vec!["type".into(), "f".into()].into()
        );

        assert_eq!(
            val(Expr::Suffix(vec!["php".into(), "js".into()])),
            vec![
                "suffix".into(),
                vec![
                    Value::ByteString("php".into()),
                    Value::ByteString("js".into())
                ]
                .into()
            ]
            .into()
        );

        assert_eq!(
            val(Expr::Since(SinceTerm::ObservedClock(ClockSpec::null()))),
            vec!["since".into(), "c:0:0".into(), "oclock".into()].into()
        );
    }
}
