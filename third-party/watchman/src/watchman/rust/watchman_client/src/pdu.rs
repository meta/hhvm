/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! This module defines the request and response PDU types used by the
//! watchman protocol.

use std::fmt;
use std::path::PathBuf;

use serde::Deserialize;
use serde::Serialize;
use serde::Serializer;
use serde_bser::value::Value;

use crate::expr::Expr;

#[derive(Deserialize, Debug)]
pub struct GetVersionResponse {
    pub version: String,
}

#[derive(Deserialize, Debug)]
pub struct WatchListResponse {
    pub roots: Vec<PathBuf>,
}

/// The `get-sockname` command response
#[derive(Deserialize, Debug)]
pub struct GetSockNameResponse {
    pub version: String,
    pub sockname: Option<PathBuf>,
    pub error: Option<String>,
}

/// The `clock` command response
#[derive(Deserialize, Debug)]
pub struct ClockResponse {
    pub version: String,
    pub clock: ClockSpec,
}

/// The `clock` command request.
#[derive(Serialize, Debug)]
pub struct ClockRequest(pub &'static str, pub PathBuf, pub ClockRequestParams);

#[derive(Serialize, Debug)]
pub struct ClockRequestParams {
    #[serde(skip_serializing_if = "SyncTimeout::is_disabled", default)]
    pub sync_timeout: SyncTimeout,
}

/// The `get-config` command request
#[derive(Serialize, Debug)]
pub struct GetConfigRequest(pub &'static str, pub PathBuf);

/// An incomplete, but typed, representation of the Watchman config file,
/// which usually lives in /etc/watchman.json. Add new fields as they're
/// needed, and it might be worth someday exposing the serde_bser::Value
/// directly so callers can parse it however they want.
#[derive(Deserialize, Debug)]
pub struct WatchmanConfig {
    pub ignore_dirs: Option<Vec<PathBuf>>,
}

/// The `get-config` command response
#[derive(Deserialize, Debug)]
pub struct GetConfigResponse {
    pub version: String,
    pub config: WatchmanConfig,
}

/// Parameters used by `state-enter` and `state-leave` commands.
#[derive(Serialize, Debug)]
pub struct StateEnterLeaveParams<'a> {
    /// The name of the state being asserted
    pub name: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub metadata: Option<Value>,
    #[serde(skip_serializing_if = "SyncTimeout::is_default")]
    pub sync_timeout: SyncTimeout,
}

/// The `state-enter` or `state-leave` request.
/// You should use `Client::state_enter` rather than directly constructing
/// this type.
#[derive(Serialize, Debug)]
pub struct StateEnterLeaveRequest<'a>(pub &'static str, pub PathBuf, pub StateEnterLeaveParams<'a>);

/// The `state-enter` response
#[derive(Deserialize, Debug)]
pub struct StateEnterLeaveResponse {
    /// The watchman server version
    pub version: String,
}

/// The `watch-project` command request.
/// You should use `Client::resolve_root` rather than directly
/// constructing this type.
#[derive(Serialize, Debug)]
pub struct WatchProjectRequest(pub &'static str, pub PathBuf);

/// The `watch-project` response
#[derive(Deserialize, Debug)]
pub struct WatchProjectResponse {
    /// The watchman server version
    pub version: String,
    /// The path relative to the root of the project; if not none,
    /// this must be passed to QueryRequestCommon::relative_root
    pub relative_path: Option<PathBuf>,
    /// The root of the watched project
    pub watch: PathBuf,
    /// The watcher that the server is using to monitor this path
    pub watcher: String,
}

/// When using the `path` generator, this specifies a path to be
/// examined.
/// <https://meta.github.io/watchman/docs/file-query.html#path-generator>
#[derive(Serialize, Clone, Debug)]
#[serde(untagged)]
pub enum PathGeneratorElement {
    RecursivePath(PathBuf),
    ConstrainedDepth { path: PathBuf, depth: i64 },
}

/// The `query` request
#[derive(Serialize, Clone, Debug)]
pub struct QueryRequest(pub &'static str, pub PathBuf, pub QueryRequestCommon);

#[allow(clippy::trivially_copy_pass_by_ref)]
fn is_false(v: &bool) -> bool {
    !*v
}

#[derive(Serialize, Clone, Debug)]
#[serde(into = "i64")]
pub struct SettleDurationMs(pub std::time::Duration);

impl From<std::time::Duration> for SettleDurationMs {
    fn from(duration: std::time::Duration) -> Self {
        Self(duration)
    }
}

impl From<SettleDurationMs> for i64 {
    fn from(val: SettleDurationMs) -> Self {
        val.0.as_millis() as i64
    }
}

#[derive(Serialize, Clone, Debug, Default)]
#[serde(into = "i64")]
pub enum SyncTimeout {
    /// Use the default cookie synchronization timeout
    #[default]
    Default,
    /// Disable the use of a sync cookie.
    /// This can save ~15ms of latency, but may result in
    /// results from an outdated view of the filesystem.
    /// It is safe to use after you have performed a synchronized
    /// query or clock call, so that you can guarantee that the
    /// server is at least as current as the time that you started
    /// your processing.
    DisableCookie,
    /// Specify a timeout for the sync cookie.  You should not
    /// need to override the default value in most cases.
    /// Note that the server has millisecond level granularity
    /// for the timeout.
    Duration(std::time::Duration),
}

impl SyncTimeout {
    fn is_default(&self) -> bool {
        match self {
            Self::Default => true,
            _ => false,
        }
    }

    fn is_disabled(&self) -> bool {
        match self {
            Self::DisableCookie => true,
            _ => false,
        }
    }
}

impl From<std::time::Duration> for SyncTimeout {
    fn from(duration: std::time::Duration) -> Self {
        let millis = duration.as_millis();
        if millis == 0 {
            Self::DisableCookie
        } else {
            Self::Duration(duration)
        }
    }
}

impl From<SyncTimeout> for i64 {
    fn from(val: SyncTimeout) -> Self {
        match val {
            // This is only really here because the `ClockRequestParams` PDU
            // treats a missing sync_timeout as `DisableCookie`, whereas
            // the `QueryRequestCommon` PDU treats it as `Default`.
            // We don't really know for sure what the server will use for
            // its default value as it may potentially be changed in a future
            // revision of the server, but for the sake of having reasonable
            // default behavior, we use the current default sync timeout here.
            // We're honestly not likely to change this, so this should be fine.
            // The server uses 1 minute; the value here is expressed in milliseconds.
            SyncTimeout::Default => 60_000,
            SyncTimeout::DisableCookie => 0,
            SyncTimeout::Duration(d) => d.as_millis() as i64,
        }
    }
}

/// The query parameters.
/// There are a large number of fields that influence the behavior.
///
/// A query consists of three phases:
/// 1. Candidate generation
/// 2. Result filtration (using the `expression` term)
/// 3. Result rendering
///
/// The generation phase is explained in detail here:
/// <https://meta.github.io/watchman/docs/file-query.html#generators>
///
/// Note that it is legal to combine multiple generators but that it
/// is often undesirable to do so.
/// Not specifying a generator results in the default "all-files" generator
/// being used to iterate all known files.
///
/// The filtration and expression syntax is explained here:
/// <https://meta.github.io/watchman/docs/file-query.html#expressions>
#[derive(Serialize, Default, Clone, Debug)]
pub struct QueryRequestCommon {
    /// If set, enables the glob generator and specifies a set of globs
    /// that will be expanded into a list of file names and then filtered
    /// according to the expression field.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub glob: Option<Vec<String>>,

    /// If using the glob generator and set to true, do not treat the backslash
    /// character as an escape sequence
    #[serde(default, skip_serializing_if = "is_false")]
    pub glob_noescape: bool,

    /// If using the glob generator and set to true, include files whose basename
    /// starts with `.` in the results. The default behavior for globs is to
    /// exclude those files from the results.
    #[serde(default, skip_serializing_if = "is_false")]
    pub glob_includedotfiles: bool,

    /// If set, enables the use of the `path` generator.
    /// <https://meta.github.io/watchman/docs/file-query.html#path-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<Vec<PathGeneratorElement>>,

    /// If set, enables the use of the `suffix` generator, and specifies the
    /// list of filename suffixes.
    /// In virtualized filesystems this can result in an expensive O(project)
    /// filesystem walk, so it is strongly recommended that you scope this to
    /// a relatively shallow subdirectory.
    ///
    /// <https://meta.github.io/watchman/docs/file-query.html#suffix-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub suffix: Option<Vec<PathBuf>>,

    /// If set, enables the use of the `since` generator and specifies the last
    /// time you queried the server and for which you wish to receive a delta of
    /// changes.
    /// You will typically thread the QueryResult.clock field back to a subsequent
    /// since query to process the continuity of matching file changes.
    /// <https://meta.github.io/watchman/docs/file-query.html#since-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub since: Option<Clock>,

    /// if set, indicates that all input paths are relative to this subdirectory
    /// in the project, and that all returned filenames will also be relative to
    /// this subdirectory.
    /// In large virtualized filesystems it is undesirable to leave this set to
    /// None as it makes it more likely that you will trigger an O(project)
    /// filesystem walk.
    /// This field is set automatically from the ResolvedRoot when you perform queries
    /// using Client::query.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub relative_root: Option<PathBuf>,

    /// If set, specifies the expression to use to filter the candidate matches
    /// produced by the selected query generator.
    /// Each candidate is visited in turn and has the expression applied.
    /// Candidates for which the expression evaluates as true will be included
    /// in the returned list of files.
    /// If left unspecified, the server will assume `Expr::True`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub expression: Option<Expr>,

    /// Specifies the list of fields names returned by the server.
    /// The `name` field should be considered a required field and is the cheapest
    /// field to return.
    /// Depending on the watcher implementation, other metadata has varying cost.
    /// In general, avoid querying `size` and `mode` fields and instead prefer to
    /// query `content.sha1hex` and `type` instead to avoid materializing inodes
    /// in a virtualized filesystem.
    pub fields: Vec<&'static str>,

    /// If true you indicate that you know how to 100% correctly deal with a fresh
    /// instance result set.  It is strongly recommended that you leave this
    /// option alone as it is a common source of cache invalidation and divergence
    /// issues for clients.
    #[serde(default, skip_serializing_if = "is_false")]
    pub empty_on_fresh_instance: bool,

    /// If true you indicate that you don't want watchman to query for or
    /// return the changed files. This is most helpful if you'd quickly
    /// like the saved-state but don't care about the changed files.
    #[serde(default, skip_serializing_if = "is_false")]
    pub omit_changed_files: bool,

    /// When requesting saved state information via SavedStateClockData, if
    /// `fail_if_no_saved_state` is set to true, the server will generate a
    /// query error in the case that the merge base change and no appropriate
    /// saved state could be found.
    /// Otherwise, the default behavior is to perform a normal watchman since
    /// query that may return a large number of changed files.
    #[serde(default, skip_serializing_if = "is_false")]
    pub fail_if_no_saved_state: bool,

    /// If true, treat filenames as case sensitive even on filesystems that otherwise
    /// appear to be case insensitive.
    /// This can improve performance of directory traversal in queries by turning
    /// O(directory-size) operations into an O(1) hash lookup.
    /// <https://meta.github.io/watchman/docs/cmd/query.html#case-sensitivity>
    #[serde(default, skip_serializing_if = "is_false")]
    pub case_sensitive: bool,

    /// If set, override the default synchronization timeout.
    /// The timeout controls how long the server will wait to observe a cookie
    /// file through the notification stream.
    /// If the timeout is reached, the query will fail.
    ///
    /// Specify `SyncTimeout::DisableCookie` to tell the server not to use a sync
    /// cookie.  **Disabling sync cookies means that your query results may be
    /// slightly out of date**.  You can safely perform a query with sync cookies
    /// disabled if you have explicitly synchronized.  For example, you can perform a
    /// synchronized `Client::clock` call at the start of your processing run
    /// to ensure that the server is current up to that point in time,
    /// and then issue a large volume of additional queries with the sync cookie
    /// disabled and save approximately ~15ms of latency per query.
    ///
    /// ## See also:
    /// * <https://meta.github.io/watchman/docs/cookies.html>
    /// * <https://meta.github.io/watchman/docs/cmd/query.html#synchronization-timeout-since-21>
    #[serde(skip_serializing_if = "SyncTimeout::is_default", default)]
    pub sync_timeout: SyncTimeout,

    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub settle_period: Option<SettleDurationMs>,

    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub settle_timeout: Option<SettleDurationMs>,

    /// If set to true, when mixing generators (not recommended), dedup results by filename.
    /// This defaults to false.  When not enabled, if multiple generators match
    /// the same file, it will appear twice in the result set.
    /// Turning on dedup_results will increase the memory cost of processing a query
    /// and build an O(result-size) hash map to dedup the results.
    #[serde(default, skip_serializing_if = "is_false")]
    pub dedup_results: bool,

    /// Controls the duration that the server will wait to obtain a lock on the
    /// filesystem view.
    /// You should not normally need to change this.
    /// <https://meta.github.io/watchman/docs/cmd/query.html#lock-timeout>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub lock_timeout: Option<i64>,

    /// If set, records the request_id in internal performance sampling data.
    /// It is also exported through the environment as HGREQUESTID so that
    /// the context of the request can be passed down to any child mercurial
    /// processes that might be spawned as part of processing source control
    /// aware queries.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub request_id: Option<String>,

    /// If this is set Watchman should guarantee that events are sent for
    /// directories. When this is not set watchman is known to skip sending
    /// events for directories on EdenFS repos that change across commits.
    /// Collecting these events can be slow when there were many recent commit
    /// transitions.
    #[serde(default, skip_serializing_if = "is_false")]
    pub always_include_directories: bool,
}

#[derive(Deserialize, Clone, Debug)]
pub struct QueryDebugInfo {
    pub cookie_files: Option<Vec<PathBuf>>,
}

/// Holds the result of a query.
/// The result is generic over a `F` type that you define.
/// The `F` should deserialize the list of fields in your QueryRequestCommon
/// struct.
#[derive(Deserialize, Clone, Debug)]
pub struct QueryResult<F>
where
    F: std::fmt::Debug + Clone,
{
    /// The version of the watchman server
    pub version: String,
    /// If true, indicates that this result set represents the
    /// total set of possible matches.  Otherwise the results should be
    /// considered to be incremental since your last since query.
    /// If is_fresh_instance is true you MUST arrange to forget about
    /// any files not included in the list of files in this QueryResult
    /// otherwise you risk diverging your state.
    #[serde(default)]
    pub is_fresh_instance: bool,

    /// Holds the list of matching files from the query
    pub files: Option<Vec<F>>,

    /// The clock value at the time that these results were generated
    pub clock: Clock,

    #[serde(rename = "state-enter")]
    #[doc(hidden)]
    pub state_enter: Option<String>,

    #[serde(rename = "state-leave")]
    #[doc(hidden)]
    pub state_leave: Option<String>,
    #[serde(rename = "metadata")]
    pub state_metadata: Option<Value>,

    /// When using source control aware queries with saved
    /// state configuration, this field holds metadata from
    /// the save state storage engine.
    #[serde(rename = "saved-state-info")]
    pub saved_state_info: Option<Value>,

    pub debug: Option<QueryDebugInfo>,
}

#[derive(Serialize, Default, Clone, Debug)]
pub struct SubscribeRequest {
    /// If set, enables the use of the `since` generator and specifies the last
    /// time you queried the server and for which you wish to receive a delta of
    /// changes.
    /// You will typically thread the QueryResult.clock field back to a subsequent
    /// since query to process the continuity of matching file changes.
    /// <https://meta.github.io/watchman/docs/file-query.html#since-generator>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub since: Option<Clock>,

    /// if set, indicates that all input paths are relative to this subdirectory
    /// in the project, and that all returned filenames will also be relative to
    /// this subdirectory.
    /// In large virtualized filesystems it is undesirable to leave this set to
    /// None as it makes it more likely that you will trigger an O(project)
    /// filesystem walk.
    /// This field is set automatically from the ResolvedRoot when you perform queries
    /// using Client::query.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub relative_root: Option<PathBuf>,

    /// If set, specifies the expression to use to filter the candidate matches
    /// produced by the selected query generator.
    /// Each candidate is visited in turn and has the expression applied.
    /// Candidates for which the expression evaluates as true will be included
    /// in the returned list of files.
    /// If left unspecified, the server will assume `Expr::True`.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub expression: Option<Expr>,

    /// Specifies the list of fields names returned by the server.
    /// The `name` field should be considered a required field and is the cheapest
    /// field to return.
    /// Depending on the watcher implementation, other metadata has varying cost.
    /// In general, avoid querying `size` and `mode` fields and instead prefer to
    /// query `content.sha1hex` and `type` instead to avoid materializing inodes
    /// in a virtualized filesystem.
    pub fields: Vec<&'static str>,

    /// If true you indicate that you know how to 100% correctly deal with a fresh
    /// instance result set.  It is strongly recommended that you leave this
    /// option alone as it is a common source of cache invalidation and divergence
    /// issues for clients.
    #[serde(default, skip_serializing_if = "is_false")]
    pub empty_on_fresh_instance: bool,

    /// If true, treat filenames as case sensitive even on filesystems that otherwise
    /// appear to be case insensitive.
    /// This can improve performance of directory traversal in queries by turning
    /// O(directory-size) operations into an O(1) hash lookup.
    /// <https://meta.github.io/watchman/docs/cmd/query.html#case-sensitivity>
    #[serde(default, skip_serializing_if = "is_false")]
    pub case_sensitive: bool,

    /// In some circumstances it is desirable for a client to observe the creation of
    /// the control files at the start of a version control operation. You may specify
    /// that you want this behavior by passing the defer_vcs flag to your
    /// subscription command invocation
    /// <https://meta.github.io/watchman/docs/cmd/subscribe.html#filesystem-settling>
    #[serde(default, skip_serializing_if = "is_false")]
    pub defer_vcs: bool,

    /// The `defer` field specifies a list of state names for which the subscriber
    /// wishes to defer the notification stream.
    /// <https://meta.github.io/watchman/docs/cmd/subscribe.html#defer>
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub defer: Vec<&'static str>,

    /// The `drop` field specifies a list of state names for which the subscriber
    /// wishes to discard the notification stream.
    /// <https://meta.github.io/watchman/docs/cmd/subscribe.html#drop>
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub drop: Vec<&'static str>,
}

#[derive(Serialize, Clone, Debug)]
pub struct SubscribeCommand(
    pub &'static str,
    pub PathBuf,
    pub String,
    pub SubscribeRequest,
);

/// Returns information about the state of the watch at the time the
/// subscription was initiated.
#[derive(Deserialize, Debug)]
pub struct SubscribeResponse {
    pub version: String,
    #[allow(unused)] // TODO unused warning after rustc upgrade
    subscribe: String,

    /// The clock at initiation time.
    pub clock: Clock,

    /// The set of asserted states at watch initiation time.
    /// This is useful in the case where you need to reason
    /// about the states and may have connected after the
    /// StateEnter was generated but prior to the StateLeave
    #[serde(default, rename = "asserted-states")]
    pub asserted_states: Vec<String>,

    /// When using source control aware queries with saved
    /// state configuration, this field holds metadata from
    /// the save state storage engine.
    #[serde(rename = "saved-state-info")]
    pub saved_state_info: Option<Value>,
}

/// The `trigger` command request.
///
/// The fields are explained in detail here:
/// <https://meta.github.io/watchman/docs/cmd/trigger#extended-syntax>
#[derive(Deserialize, Serialize, Default, Clone, Debug)]
pub struct TriggerRequest {
    /// Defines the name of the trigger.
    pub name: String,

    /// Specifies the command to invoke.
    pub command: Vec<String>,

    /// It true, matching files (up to system limits) will be added to the
    /// command's execution args.
    #[serde(default, skip_serializing_if = "is_false")]
    pub append_files: bool,

    /// Specifies the expression used to filter candidate matches.
    #[serde(skip_serializing_if = "Option::is_none", skip_deserializing)]
    pub expression: Option<Expr>,

    /// Configure the way `stdin` is configured for the executed trigger.
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        serialize_with = "TriggerStdinConfig::serialize",
        skip_deserializing
    )]
    pub stdin: Option<TriggerStdinConfig>,

    /// Specifies a file to write the output stream to.  Prefix with `>` to
    /// overwrite and `>>` to append.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stdout: Option<String>,

    /// Specifies a file to write the error stream to.  Prefix with `>` to
    /// overwrite and `>>` to append.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stderr: Option<String>,

    /// Specifies a limit on the number of files reported on stdin when stdin is
    /// set to hold the set of matched files.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_files_stdin: Option<u64>,

    /// Specifies the working directory that will be set prior to spawning the
    /// process. The default is to set the working directory to the watched
    /// root. The value of this property is a string that will be interpreted
    /// relative to the watched root.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub chdir: Option<String>,

    /// Since 3.4. Evaluates triggers with respect to a path within a watched root.
    ///
    /// See <https://meta.github.io/watchman/docs/cmd/trigger#relative-roots>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub relative_root: Option<PathBuf>,
}

#[derive(Clone, Debug, Default)]
pub enum TriggerStdinConfig {
    #[default]
    DevNull,
    FieldNames(Vec<String>),
    NamePerLine,
}

impl TriggerStdinConfig {
    fn serialize<S>(this: &Option<Self>, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match this {
            Some(Self::DevNull) => serializer.serialize_str("/dev/null"),
            Some(Self::FieldNames(names)) => serializer.collect_seq(names.iter()),
            Some(Self::NamePerLine) => serializer.serialize_str("NAME_PER_LINE"),
            None => serializer.serialize_none(),
        }
    }
}

#[derive(Serialize, Clone, Debug)]
pub struct TriggerCommand(pub &'static str, pub PathBuf, pub TriggerRequest);

#[derive(Deserialize, Debug)]
pub struct TriggerResponse {
    pub version: String,
    pub disposition: String,
    pub triggerid: String,
}

#[derive(Serialize, Clone, Debug)]
pub struct TriggerDelCommand(pub &'static str, pub PathBuf, pub String);

#[derive(Deserialize, Debug)]
pub struct TriggerDelResponse {
    pub version: String,
    pub deleted: bool,
    pub trigger: String,
}

#[derive(Serialize, Clone, Debug)]
pub struct TriggerListCommand(pub &'static str, pub PathBuf);

#[derive(Deserialize, Debug)]
pub struct TriggerListResponse {
    pub version: String,
    pub triggers: Vec<TriggerRequest>,
}

#[derive(Serialize, Debug)]
pub struct Unsubscribe(pub &'static str, pub PathBuf, pub String);

#[derive(Deserialize, Debug)]
pub struct UnsubscribeResponse {
    pub version: String,
    pub unsubscribe: String,
}

#[derive(Serialize, Debug)]
pub struct GetAssertedStatesRequest(pub &'static str, pub PathBuf);

#[derive(Deserialize, Debug)]
pub struct State {
    pub name: String,
    pub state: String,
}

#[derive(Deserialize, Debug)]
pub struct GetAssertedStatesResponse {
    pub version: String,
    pub root: String,
    pub states: Vec<State>,
}

/// A `Clock` is used to refer to a logical point in time.
/// Internally, watchman maintains a monotonically increasing tick counter
/// along with some additional data to detect A-B-A style situations if
/// eg: the watchman server is restarted.
///
/// Clocks are important when using the recency index with the `since`
/// generator.
///
/// A clock can also encoded metadata describing the state of source
/// control to work with source control aware queries:
/// <https://meta.github.io/watchman/docs/scm-query.html>
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(untagged)]
pub enum Clock {
    /// Just a basic ClockSpec
    Spec(ClockSpec),
    /// A clock embedding additional source control information
    ScmAware(FatClockData),
}

/// The fundamental clock specifier string.
/// The contents of the string should be considered to be opaque to
/// the client as the server occasionally evolves the meaning of
/// the clockspec and its format is expressly not a stable API.
///
/// In particular, there is no defined way for a client to reason
/// about the relationship between any two ClockSpec's.
///
/// <https://meta.github.io/watchman/docs/clockspec.html>
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(untagged)]
pub enum ClockSpec {
    StringClock(String),
    UnixTimestamp(i64),
}

/// Construct a null clockspec
impl Default for ClockSpec {
    fn default() -> Self {
        Self::null()
    }
}

impl ClockSpec {
    /// Construct a null clockspec.
    /// This indicates a time before any changes occurred and will
    /// cause a `since` generator based query to emit a fresh instance
    /// result set that contains all possible matches.
    /// It is appropriate to use a null clock in cases where you are
    /// starting up from scratch and don't have a saved clock value
    /// to use as the basis for your query.
    pub fn null() -> Self {
        Self::StringClock("c:0:0".to_string())
    }

    /// Construct a named cursor clockspec.
    ///
    /// Using a named cursor causes the server to maintain the name -> clock
    /// value mapping on the behalf of your tool.  This frees your client
    /// from the need to manage storing of the clock between queries but
    /// does require an exclusive lock for the duration of the query, which
    /// serializes the query with all other clients.
    ///
    /// The namespace is per watched project so your cursor name must be
    /// unique enough to not collide with other tools that use this same
    /// feature.
    ///
    /// There is no way to clear the value associated with a named cursor.
    ///
    /// The first time you use a named cursor, it has an effective value
    /// of the null clock.
    ///
    /// We do not recommend using named cursors because of the exclusive
    /// lock requirement.
    pub fn named_cursor(cursor: &str) -> Self {
        Self::StringClock(format!("n:{}", cursor))
    }

    /// A clock specified as a unix timestamp.
    /// The watchman server will never generate a clock in this form,
    /// but will accept them in `since` generator based queries.
    /// Using UnixTimeStamp is discouraged as it has granularity of
    /// 1 second and will often result in over-reporting the same events
    /// when they happen in the same second.
    pub fn unix_timestamp(time_t: i64) -> Self {
        Self::UnixTimestamp(time_t)
    }
}

impl From<ClockSpec> for Value {
    fn from(val: ClockSpec) -> Self {
        match val {
            ClockSpec::StringClock(st) => Value::Utf8String(st),
            ClockSpec::UnixTimestamp(ts) => Value::Integer(ts),
        }
    }
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://meta.github.io/watchman/docs/scm-query.html>
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct FatClockData {
    pub clock: ClockSpec,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub scm: Option<ScmAwareClockData>,
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://meta.github.io/watchman/docs/scm-query.html>
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct ScmAwareClockData {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mergebase: Option<String>,
    #[serde(rename = "mergebase-with")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mergebase_with: Option<String>,

    #[serde(rename = "saved-state")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub saved_state: Option<SavedStateClockData>,
}

/// Holds extended clock data that includes source control aware
/// query metadata.
/// <https://meta.github.io/watchman/docs/scm-query.html>
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct SavedStateClockData {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub storage: Option<String>,
    #[serde(rename = "commit-id")]
    #[serde(skip_serializing_if = "Option::is_none")]
    pub commit: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub config: Option<Value>,
}

/// Reports the content SHA1 hash for a file.
/// Since computing the hash can fail, this struct can also represent
/// the error that happened during hash computation.
#[derive(Deserialize, Debug, Clone, PartialEq)]
#[serde(untagged)]
pub enum ContentSha1Hex {
    /// The 40-hex-digit SHA1 content hash of the file contents
    Hash(String),
    /// The error that occurred while trying to determine the hash
    Error { error: String },
    /// Value if the file was deleted.
    /// Note that this is distinct from the case where watchman believes
    /// that the file exists and where some other process unlinks it before
    /// watchman can compute the hash: in that racy scenario, the value
    /// will be `ContentSha1Hex::Error(_)`.
    None,
}

/// Encodes the file type field returned in query results and
/// specified in expression terms.
///
/// <https://meta.github.io/watchman/docs/expr/type.html>
///
/// Use this in your query file struct like this:
///
/// ```
/// use serde::Deserialize;
/// use watchman_client::prelude::*;
/// #[derive(Deserialize, Debug, Clone)]
/// struct NameAndType {
///     name: std::path::PathBuf,
///     #[serde(rename = "type")]
///     file_type: FileType,
/// }
/// ```
#[derive(Serialize, Deserialize, Debug, Clone, Copy)]
#[serde(from = "String", into = "String")]
pub enum FileType {
    BlockSpecial,
    CharSpecial,
    Directory,
    Regular,
    Fifo,
    Symlink,
    Socket,
    SolarisDoor,
    Unknown,
}

impl fmt::Display for FileType {
    fn fmt(&self, f: &mut fmt::Formatter) -> Result<(), fmt::Error> {
        f.write_str(&String::from(*self))
    }
}

impl From<String> for FileType {
    fn from(s: String) -> Self {
        match s.as_ref() {
            "b" => Self::BlockSpecial,
            "c" => Self::CharSpecial,
            "d" => Self::Directory,
            "f" => Self::Regular,
            "p" => Self::Fifo,
            "l" => Self::Symlink,
            "s" => Self::Socket,
            "D" => Self::SolarisDoor,
            "?" => Self::Unknown,
            unknown => panic!("Watchman Server returned impossible file type {}", unknown),
        }
    }
}

impl From<FileType> for String {
    fn from(val: FileType) -> Self {
        match val {
            FileType::BlockSpecial => "b",
            FileType::CharSpecial => "c",
            FileType::Directory => "d",
            FileType::Regular => "f",
            FileType::Fifo => "p",
            FileType::Symlink => "l",
            FileType::Socket => "s",
            FileType::SolarisDoor => "D",
            FileType::Unknown => "?",
        }
        .to_string()
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use serde_bser::value::Value;

    use super::*;
    use crate::bunser;

    fn convert_bser_value<T>(input: Value) -> T
    where
        T: serde::de::DeserializeOwned,
    {
        let binary = serde_bser::ser::serialize(Vec::new(), input).unwrap();
        bunser(&binary).unwrap()
    }

    #[test]
    fn test_content_sha1hex_hash() {
        let value: ContentSha1Hex =
            convert_bser_value("e820c2c600a36f05ba905cf1bf32c4834e804e22".into());
        assert_eq!(
            value,
            ContentSha1Hex::Hash("e820c2c600a36f05ba905cf1bf32c4834e804e22".into())
        );
    }

    #[test]
    fn test_content_sha1hex_error() {
        let mut error_obj: HashMap<String, Value> = HashMap::new();
        error_obj.insert("error".to_string(), "out of cookies".into());

        let value: ContentSha1Hex = convert_bser_value(error_obj.into());
        assert_eq!(
            value,
            ContentSha1Hex::Error {
                error: "out of cookies".into()
            }
        );
    }

    #[test]
    fn test_content_sha1hex_none() {
        let value: ContentSha1Hex = convert_bser_value(Value::Null);
        assert_eq!(value, ContentSha1Hex::None);
    }
}
