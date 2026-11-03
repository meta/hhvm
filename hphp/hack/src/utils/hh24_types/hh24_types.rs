// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

//! Common types used in the HH24 Hack typechecker rearchitecture.

// Common impls for types which wrap a hash value represented by u64.
macro_rules! u64_hash_wrapper_impls {
    ($name:ident) => {
        impl $name {
            #[inline]
            pub fn from_u64(hash: u64) -> Self {
                Self(hash)
            }
            #[inline]
            pub fn as_u64(self) -> u64 {
                self.0
            }
        }

        impl std::fmt::Debug for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(f, concat!(stringify!($name), "({:x})"), self.0)
            }
        }

        impl std::str::FromStr for $name {
            type Err = std::num::ParseIntError;
            fn from_str(s: &str) -> Result<Self, Self::Err> {
                Ok(Self(u64::from_str_radix(s, 16)?))
            }
        }

        impl rusqlite::ToSql for $name {
            fn to_sql(&self) -> rusqlite::Result<rusqlite::types::ToSqlOutput<'_>> {
                Ok(rusqlite::types::ToSqlOutput::from(self.0 as i64))
            }
        }

        impl rusqlite::types::FromSql for $name {
            fn column_result(
                value: rusqlite::types::ValueRef<'_>,
            ) -> rusqlite::types::FromSqlResult<Self> {
                Ok(Self(value.as_i64()? as u64))
            }
        }
    };
}

/// TODO(ljw): add backtraces to the three expected cases.
/// But let's hold off until we've adopted thiserror 1.0.34 and rustc post backtrace stabilization
#[derive(thiserror::Error, Debug)]
pub enum HhError {
    /// This error is used for all unexpected scenarios - anything where we want a callstack
    /// and telemetry. This includes disk io errors, json parsing errors.
    #[error("Unexpected: {0:#}")]
    Unexpected(anyhow::Error),

    /// This means that hh_decl was asked for a decl, and it had to read it off disk, but
    /// what it read off disk is different from what it expected in the naming table.
    /// This means that the disk has changed in a material way since the last time anyone
    /// invoked "hh_decl change", or its user-facing caller "hh update". Therefore,
    /// any facts or derived facts (shallow or folded decs, or depgraph edges) that we
    /// attempted to deduce based on reading a disk file run the risk of being invalid in ways
    /// that we won't subsequently be able to invalidate. Therefore, we must not commit such
    /// facts to memory or anywhere. In the face of such a situation, no useful work is
    /// possible by hh_decl nor by the caller of it (hh_fanout, hh_worker, hh). The only thing
    /// that hh_decl or its callers can do in this situation is propagate the DiskChanged error
    /// upwards for now, and terminate themselves. What's needed for work to proceed is
    /// to do "hh_decl change" with the affected files. More typically, this will be done
    /// by "hh update", which will query watchman for all modified files, then invoke "hh_decl change"
    /// for them, then invoke "hh_fanout change" for modified symbols. Note that the only
    /// component which can recover from DiskChanged is "hh --retry-on-disk-changed", which
    /// sees a DiskChanged error reported from one of its subcomponents (hh_decl, hh_fanout, hh_worker)
    /// and does that "hh update" remediation step itself.
    #[error("Disk changed: {0} - do hh_decl change then restart the operation. [{1}]")]
    DiskChanged(std::path::PathBuf, String),

    /// This means that hh_decl was asked for a decl by some component (hh_fanout, hh_worker, hh)
    /// but some concurrent process had already done "hh_decl change" to inform it of modified
    /// symbols. In other words, hh_decl is "in the future" and knows about changes on disk
    /// that its caller doesn't yet know about. In such a situation it's unsafe for the caller
    /// to continue -- any facts or derived facts that the caller attempts to store will be
    /// invalid in ways it can't recover from. The only action the caller can do is terminate
    /// itself, and trust that someone will restart it. For cases purely within "hh check" this
    /// situation won't arise. Where it will arise is if someone did "hh --type-at-pos ..." in the
    /// background, and then also did "hh update", and the hh update might have updated hh_decl
    /// in such a way that the type-at-pos worker is unable to proceed. (If someone had done
    /// "hh --type-at-pos --retry-on-disk-changed" then after the worker terminated with this error,
    // then hh would know to restart it.)
    #[error("Hh_decl changed its checksum: {0:?} - restart the operation. [{1}]")]
    ChecksumChanged(Checksum, String),

    /// This means that hh_decl was told to stop. This error is our chief signalling mechanism for
    /// getting concurrent workers to stop too: they are (presumably) spending their time reading
    /// hh_decl, and once we tell hh_decl to stop then they'll soon get the message, and know
    /// to shut down.
    #[error("Hh_decl stopped - abandon the operation. [{0}]")]
    Stopped(String),
}

/// TODO(ljw): once we adopt thiserror 1.0.34 and anyhow 1.0.64, then anyhow
/// stacks will always be present, and we'll have no need for peppering our
/// codebase with .hh_context("desc") to make up for their lack. At that time,
/// let's delete the HhErrorContext trait and all calls to it.
pub trait HhErrorContext<T> {
    fn hh_context(self, context: &'static str) -> Result<T, HhError>;
}

impl<T> HhErrorContext<T> for Result<T, HhError> {
    fn hh_context(self, ctx: &'static str) -> Result<T, HhError> {
        match self {
            Ok(r) => Ok(r),
            Err(HhError::Unexpected(err)) => Err(HhError::Unexpected(err.context(ctx))),
            Err(HhError::DiskChanged(path, ctx0)) => {
                Err(HhError::DiskChanged(path, format!("{}\n{}", ctx, ctx0)))
            }
            Err(HhError::ChecksumChanged(checksum, ctx0)) => Err(HhError::ChecksumChanged(
                checksum,
                format!("{}\n{}", ctx, ctx0),
            )),
            Err(HhError::Stopped(ctx0)) => Err(HhError::Stopped(format!("{}\n{}", ctx, ctx0))),
        }
    }
}

impl<T> HhErrorContext<T> for Result<T, std::io::Error> {
    #[inline(never)]
    fn hh_context(self, context: &'static str) -> Result<T, HhError> {
        self.map_err(|err| HhError::Unexpected(anyhow::Error::new(err).context(context)))
    }
}

impl<T> HhErrorContext<T> for Result<T, serde_json::error::Error> {
    #[inline(never)]
    fn hh_context(self, context: &'static str) -> Result<T, HhError> {
        self.map_err(|err| HhError::Unexpected(anyhow::Error::new(err).context(context)))
    }
}

impl<T> HhErrorContext<T> for Result<T, anyhow::Error> {
    #[inline(never)]
    fn hh_context(self, context: &'static str) -> Result<T, HhError> {
        self.map_err(|err| HhError::Unexpected(err.context(context)))
    }
}

/// Checksum is used to characterize state of every decl in the repository:
/// if a decl is added, removed, moved from one file, changed, then the overall
/// checksum of the repository will change.
#[derive(Copy, Clone, Hash, PartialEq, Eq, Default)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct Checksum(pub u64);
u64_hash_wrapper_impls! { Checksum }

impl Checksum {
    pub fn addremove(
        &mut self,
        symbol_hash: ToplevelSymbolHash,
        decl_hash: DeclHash,
        path: &relative_path::RelativePath,
    ) {
        // CARE! This implementation must be identical to that in rust_decl_ffi.rs
        // I wrote it out as a separate copy because I didn't want hh_server to take a dependency
        // upon hh24_types
        self.0 ^= hh_hash::hash(&(symbol_hash, decl_hash, path));
    }
}

#[derive(Clone, Debug)]
#[derive(serde::Deserialize, serde::Serialize)]
pub struct RichChecksum {
    pub checksum: Checksum,
    pub timestamp: Timestamp,
    pub example_symbol: String,
}

impl RichChecksum {
    pub fn to_brief_string(&self) -> String {
        format!(
            "RichChecksum({:x}@{}@{})",
            self.checksum,
            self.timestamp.unix_epoch_secs(),
            self.example_symbol
        )
    }
}

impl std::str::FromStr for RichChecksum {
    type Err = ParseRichChecksumError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let s = s.trim_start_matches("RichChecksum(");
        let s = s.trim_end_matches(')');
        let mut iter = s.split('@');
        match (iter.next(), iter.next(), iter.next(), iter.next()) {
            (Some(checksum), Some(timestamp), Some(example_symbol), None) => Ok(Self {
                checksum: checksum
                    .parse()
                    .map_err(ParseRichChecksumError::InvalidChecksum)?,
                timestamp: timestamp
                    .parse()
                    .map_err(ParseRichChecksumError::InvalidTimestamp)?,
                example_symbol: String::from(example_symbol),
            }),
            _ => Err(ParseRichChecksumError::Invalid),
        }
    }
}

#[derive(thiserror::Error, Debug)]
pub enum ParseRichChecksumError {
    #[error("expected \"RichChecksum(<checksum>@<timestamp>@<example_symbol>)\"")]
    Invalid,
    #[error("{0}")]
    InvalidChecksum(#[source] std::num::ParseIntError),
    #[error("{0}")]
    InvalidTimestamp(#[source] std::num::ParseIntError),
}

/// A measurement of the system clock, useful for talking to external entities
/// like the file system or other processes. Wraps `std::time::SystemTime`, but
/// implements `serde::Serialize` and `serde::Deserialize`.
///
/// Invariant: always represents a time later than the unix epoch.
#[derive(Copy, Clone)]
pub struct Timestamp(std::time::SystemTime);

impl Timestamp {
    /// Returns the system time corresponding to "now".
    pub fn now() -> Self {
        Self(std::time::SystemTime::now())
    }

    /// Returns the system time corresponding to the unix epoch plus the given
    /// number of seconds.
    pub fn from_unix_epoch_secs(secs: u64) -> Self {
        Self(
            std::time::SystemTime::UNIX_EPOCH
                .checked_add(std::time::Duration::from_secs(secs))
                .expect("Seconds since UNIX_EPOCH too large to fit in SystemTime"),
        )
    }

    /// Returns the number of seconds elapsed between the unix epoch and this
    /// `Timestamp`.
    pub fn unix_epoch_secs(&self) -> u64 {
        self.0
            .duration_since(std::time::SystemTime::UNIX_EPOCH)
            .expect("Timestamp before UNIX_EPOCH")
            .as_secs()
    }

    /// Returns the `SystemTime` corresponding to this `Timestamp`.
    pub fn as_system_time(&self) -> std::time::SystemTime {
        self.0
    }
}

impl std::fmt::Debug for Timestamp {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Timestamp({})", self.unix_epoch_secs())
    }
}

impl std::str::FromStr for Timestamp {
    type Err = std::num::ParseIntError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let s = s.trim_start_matches("Timestamp(");
        let s = s.trim_end_matches(')');
        Ok(Self::from_unix_epoch_secs(s.parse()?))
    }
}

impl serde::Serialize for Timestamp {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_u64(self.unix_epoch_secs())
    }
}

impl<'de> serde::Deserialize<'de> for Timestamp {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Visitor;
        impl<'de> serde::de::Visitor<'de> for Visitor {
            type Value = Timestamp;

            fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(formatter, "a u64 for Timestamp")
            }
            fn visit_u64<E: serde::de::Error>(self, value: u64) -> Result<Self::Value, E> {
                Ok(Self::Value::from_unix_epoch_secs(value))
            }
        }
        deserializer.deserialize_u64(Visitor)
    }
}

/// The hash of a toplevel symbol name, as it appears in the 64bit dependency graph.
#[derive(Copy, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct ToplevelSymbolHash(u64);
u64_hash_wrapper_impls! { ToplevelSymbolHash }

impl ToplevelSymbolHash {
    pub fn new(kind: file_info::NameType, symbol: &str) -> Self {
        Self::from_byte_string(kind, symbol.as_bytes())
    }

    pub fn from_byte_string(kind: file_info::NameType, symbol: &[u8]) -> Self {
        Self(typing_deps_hash::hash1(kind.into(), symbol))
    }

    pub fn from_type(symbol: &str) -> Self {
        // Could also be a NameType::Typedef, but both Class and Typedef are
        // represented with DepType::KType. See test_dep_type_from_name_type below.
        Self::new(file_info::NameType::Class, symbol)
    }

    pub fn from_fun(symbol: &str) -> Self {
        Self::new(file_info::NameType::Fun, symbol)
    }

    pub fn from_const(symbol: &str) -> Self {
        Self::new(file_info::NameType::Const, symbol)
    }

    pub fn from_module(symbol: &str) -> Self {
        Self::new(file_info::NameType::Module, symbol)
    }

    #[inline(always)]
    pub fn to_be_bytes(self) -> [u8; 8] {
        self.0.to_be_bytes()
    }

    #[inline(always)]
    pub fn from_be_bytes(bs: [u8; 8]) -> Self {
        Self(u64::from_be_bytes(bs))
    }

    #[inline(always)]
    pub fn to_dependency_hash(self) -> DependencyHash {
        DependencyHash(self.0)
    }
}

/// The "canon hash" of a toplevel symbol name (i.e., the hash of the symbol
/// name after ASCII characters in the name have been converted to lowercase),
/// as it appears in the naming table.
#[derive(Copy, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct ToplevelCanonSymbolHash(u64);
u64_hash_wrapper_impls! { ToplevelCanonSymbolHash }

impl ToplevelCanonSymbolHash {
    pub fn new(kind: file_info::NameType, mut symbol: String) -> Self {
        symbol.make_ascii_lowercase();
        Self(typing_deps_hash::hash1(kind.into(), symbol.as_bytes()))
    }

    pub fn from_type(symbol: String) -> Self {
        // Could also be a NameType::Typedef, but both Class and Typedef are
        // represented with DepType::KType. See test_dep_type_from_name_type below.
        Self::new(file_info::NameType::Class, symbol)
    }

    pub fn from_fun(symbol: String) -> Self {
        Self::new(file_info::NameType::Fun, symbol)
    }

    pub fn from_const(symbol: String) -> Self {
        Self::new(file_info::NameType::Const, symbol)
    }

    pub fn from_module(symbol: String) -> Self {
        Self::new(file_info::NameType::Module, symbol)
    }
}

/// The hash of a toplevel symbol name, or the hash of a class member name, or
/// an Extends, Constructor, or AllMembers hash for a class name.
/// See `Typing_deps.Dep.(dependency variant)`. and Dep.make in typing_deps.ml
#[derive(Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct DependencyHash(pub u64);

impl DependencyHash {
    pub fn of_member(
        dep_type: typing_deps_hash::DepType,
        type_hash: ToplevelSymbolHash,
        member_name: &str,
    ) -> Self {
        Self(typing_deps_hash::hash2(
            dep_type,
            type_hash.0,
            member_name.as_bytes(),
        ))
    }

    pub fn of_symbol(dep_type: typing_deps_hash::DepType, type_name: &str) -> Self {
        Self(typing_deps_hash::hash1(dep_type, type_name.as_bytes()))
    }

    #[inline]
    pub fn as_u64(self) -> u64 {
        self.0
    }
}

impl std::fmt::Debug for DependencyHash {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "DependencyHash({:x})", self.0)
    }
}

impl std::str::FromStr for DependencyHash {
    type Err = std::num::ParseIntError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(Self(u64::from_str_radix(s, 16)?))
    }
}

// A `ToplevelSymbolHash` is a valid `DependencyHash`, but not all
// `DependencyHash` values represent a toplevel symbol.
impl From<ToplevelSymbolHash> for DependencyHash {
    fn from(hash: ToplevelSymbolHash) -> Self {
        Self(hash.0)
    }
}

impl From<DependencyHash> for ToplevelSymbolHash {
    fn from(hash: DependencyHash) -> Self {
        Self(hash.0)
    }
}

impl DependencyHash {
    #[inline(always)]
    pub fn to_be_bytes(self) -> [u8; 8] {
        self.0.to_be_bytes()
    }

    #[inline(always)]
    pub fn from_be_bytes(bs: [u8; 8]) -> Self {
        Self(u64::from_be_bytes(bs))
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(serde::Deserialize, serde::Serialize)]
pub struct DepGraphEdge {
    pub dependency: DependencyHash,
    pub dependent: ToplevelSymbolHash,
}

impl DepGraphEdge {
    pub fn from_u64(dependency: u64, dependent: u64) -> Self {
        Self {
            dependency: DependencyHash(dependency),
            dependent: ToplevelSymbolHash::from_u64(dependent),
        }
    }
}

impl std::str::FromStr for DepGraphEdge {
    type Err = ParseDepGraphEdgeError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut iter = s.split(':');
        match (iter.next(), iter.next(), iter.next()) {
            (Some(dependency), Some(dependent), None) => Ok(Self {
                dependency: dependency.parse()?,
                dependent: dependent.parse()?,
            }),
            _ => Err(ParseDepGraphEdgeError::Invalid(s.to_owned())),
        }
    }
}

#[derive(thiserror::Error, Debug)]
pub enum ParseDepGraphEdgeError {
    #[error("expected dependency_hash:dependent_hash format. actual \"{0}\"")]
    Invalid(String),
    #[error("{0}")]
    FromInt(#[from] std::num::ParseIntError),
}

/// The position-insensitive hash of the `Decls` produced by running the direct
/// decl parser on a file. Used in the NAMING_FILE_INFO table.
#[derive(Copy, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct FileDeclsHash(u64);
u64_hash_wrapper_impls! { FileDeclsHash }

/// The position-sensitive hash of a decl (the type signature of a toplevel
/// declaration), as it appears in the naming table. Stored in the `DECL_HASH`
/// column of the `NAMING_SYMBOLS` and `NAMING_SYMBOLS_OVERFLOW` tables.
///
/// Invariant: Each value of this type originates from calling `hh_hash::hash` on a
/// value of type `oxidized::shallow_decl_defs::Decl` (but may be subsequently
/// deserialized, loaded from the naming table, etc).
#[derive(Copy, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[derive(serde::Deserialize, serde::Serialize)]
#[derive(derive_more::UpperHex, derive_more::LowerHex)]
pub struct DeclHash(u64);
u64_hash_wrapper_impls! { DeclHash }

/// This type is for serializing an anyhow error. What you get out will print with
/// the same information as the original anyhow, but won't look quite as pretty
/// and doesn't support downcasting.
#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct StringifiedError {
    pub chain: Vec<String>, // invariant: this has at least 1 element
    pub backtrace: String,
}

impl StringifiedError {
    pub fn from_anyhow(err: anyhow::Error) -> Self {
        let chain = err.chain().map(|c| format!("{}", c)).rev().collect();
        let backtrace = format!("{:?}", err);
        Self { chain, backtrace }
    }

    pub fn to_anyhow(self) -> anyhow::Error {
        let mut e = anyhow::anyhow!("StringifiedError");
        e = e.context(self.backtrace);
        for cause in self.chain {
            e = e.context(cause);
        }
        e
    }
}

impl std::fmt::Display for StringifiedError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.chain[self.chain.len() - 1])
    }
}

#[cfg(test)]
mod tests {
    use anyhow::Context;

    use super::*; // to bring Result<T,E>.context into scope

    fn stringify_inner() -> anyhow::Result<()> {
        anyhow::bail!("oops");
    }

    fn stringify_middle() -> anyhow::Result<()> {
        stringify_inner().context("ctx_middle")
    }

    fn stringify_outer() -> anyhow::Result<()> {
        stringify_middle().context("ctx_outer")
    }

    #[test]
    fn stringify_without_backtrace() {
        match stringify_outer() {
            Ok(()) => panic!("test wanted to see an error"),
            Err(err1) => {
                let err2 = StringifiedError::from_anyhow(err1);
                let err3 = err2.to_anyhow();
                let display = format!("{}", err3);
                assert_eq!(display, "ctx_outer");
                let debug = format!("{:?}", err3);
                assert!(debug.contains("ctx_outer"));
                assert!(debug.contains("0: ctx_middle"));
                assert!(debug.contains("1: oops"));
            }
        }
    }

    #[test]
    fn test_dep_type_from_name_type() {
        assert_eq!(
            typing_deps_hash::DepType::from(file_info::NameType::Class),
            typing_deps_hash::DepType::from(file_info::NameType::Typedef)
        );
    }
}
