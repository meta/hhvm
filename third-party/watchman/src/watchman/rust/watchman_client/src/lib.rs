/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//! This crate provides a client to the watchman file watching service.
//!
//! Start with the [Connector](struct.Connector.html) struct and use
//! it to connect and return a [Client](struct.Client.html) struct,
//! [Client::resolve_root](struct.Client.html#method.resolve_root) to
//! resolve a path and initiate a watch, and then
//! [Client::query](struct.Client.html#method.query) to perform
//! a query, or [Client::subscribe](struct.Client.html#method.subscribe)
//! to subscribe to file changes in real time.
//!
//! This example shows how to connect and expand a glob from the
//! current working directory:
//!
//! ```no_run
//! use watchman_client::prelude::*;
//! #[tokio::main]
//! async fn main() -> Result<(), Box<dyn std::error::Error>> {
//!     let mut client = Connector::new().connect().await?;
//!     let resolved = client
//!         .resolve_root(CanonicalPath::canonicalize(".")?)
//!         .await?;
//!
//!     // Basic globs -> names
//!     let files = client.glob(&resolved, &["**/*.rs"]).await?;
//!     println!("files: {:#?}", files);
//!     Ok(())
//! }
//! ```
#![cfg_attr(fbcode_build, deny(warnings))]

pub mod expr;
pub mod fields;
mod named_pipe;
pub mod pdu;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::io;
use std::io::Write;
use std::marker::PhantomData;
use std::path::Path;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use bytes::Bytes;
use bytes::BytesMut;
use futures::future::FutureExt;
use futures::stream::StreamExt;
use serde_bser::de::Bunser;
use serde_bser::de::SliceRead;
pub use serde_bser::value::Value;
use thiserror::Error;
use tokio::io::AsyncRead;
use tokio::io::AsyncWrite;
use tokio::io::AsyncWriteExt;
#[cfg(unix)]
use tokio::net::UnixStream;
use tokio::process::Command;
use tokio::sync::Mutex;
use tokio::sync::mpsc::Receiver;
use tokio::sync::mpsc::Sender;
use tokio::sync::mpsc::UnboundedReceiver;
use tokio::sync::mpsc::UnboundedSender;
use tokio_util::codec::Decoder;
use tokio_util::codec::FramedRead;

/// The next id number to use when generating a subscription name
static SUB_ID: AtomicUsize = AtomicUsize::new(1);

/// `use watchman_client::prelude::*` for convenient access to the types
/// provided by this crate
pub mod prelude {
    pub use crate::CanonicalPath;
    pub use crate::Client;
    pub use crate::Connector;
    pub use crate::ResolvedRoot;
    pub use crate::expr::*;
    pub use crate::fields::*;
    pub use crate::pdu::*;
    pub use crate::query_result_type;
}

use prelude::*;

#[derive(Error, Debug)]
pub enum ConnectionLost {
    #[error("Client task exited")]
    ClientTaskExited,

    #[error("Client task failed: {0}")]
    Error(String),
}

#[derive(Error, Debug)]
pub enum Error {
    #[error("Failed to connect to Watchman: {0}")]
    ConnectionError(tokio::io::Error),

    #[error("Lost connection to watchman")]
    ConnectionLost(#[from] ConnectionLost),

    #[error(
        "While invoking the {watchman_path} CLI to discover the server connection details: {reason}, stderr=`{stderr}`"
    )]
    ConnectionDiscovery {
        watchman_path: PathBuf,
        reason: String,
        stderr: String,
    },
    #[error("The watchman server reported an error: \"{}\", while executing command: {}", .message, .command)]
    WatchmanServerError { message: String, command: String },
    #[error("The watchman server reported an error: \"{}\"", .message)]
    WatchmanResponseError { message: String },
    #[error("The watchman server didn't return a value for field `{}` in response to a `{}` command. {:?}", .fieldname, .command, .response)]
    MissingField {
        fieldname: &'static str,
        command: String,
        response: String,
    },

    #[error("Deserialization error (data: {data:x?})")]
    Deserialize {
        data: Vec<u8>,
        #[source]
        source: anyhow::Error,
    },

    #[error("Seriaization error")]
    Serialize {
        #[source]
        source: anyhow::Error,
    },

    #[error("Failed to connect to {endpoint}")]
    Connect {
        endpoint: PathBuf,
        #[source]
        source: Box<std::io::Error>,
    },
}

#[derive(Error, Debug)]
enum TaskError {
    #[error("IO Error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Task is shutting down")]
    Shutdown,

    #[error("EOF on Watchman socket")]
    Eof,

    #[error("Received a unilateral PDU from the server")]
    UnilateralPdu,

    #[error("Deserialization error (data: {data:x?})")]
    Deserialize {
        #[source]
        source: anyhow::Error,
        data: Vec<u8>,
    },
}

/// The Connector defines how to connect to the watchman server.
/// You will typically use `Connector::new` to set up the connection with
/// the environmental defaults.  You might want to override those defaults
/// in situations such as integration testing environments, or in extremely
/// latency sensitive environments where the cost of performing discovery
/// is a measurable overhead.
#[derive(Default)]
pub struct Connector {
    watchman_cli_path: Option<PathBuf>,
    unix_domain: Option<PathBuf>,
}

impl Connector {
    /// Set up the connector with the system defaults.
    /// If `WATCHMAN_SOCK` is set in the environment it will preset the
    /// local IPC socket path.
    /// Otherwise the connector will invoke the watchman CLI to perform
    /// discovery.
    pub fn new() -> Self {
        let connector = Self::default();

        if let Some(val) = std::env::var_os("WATCHMAN_SOCK") {
            connector.unix_domain_socket(val)
        } else {
            connector
        }
    }

    /// If the watchman CLI is installed in a location that is not present
    /// in the PATH environment variable, this method is used to inform
    /// the connector of its location.
    pub fn watchman_cli_path<P: AsRef<Path>>(mut self, path: P) -> Self {
        self.watchman_cli_path = Some(path.as_ref().to_path_buf());
        self
    }

    /// Specify the unix domain socket path
    pub fn unix_domain_socket<P: AsRef<Path>>(mut self, path: P) -> Self {
        self.unix_domain = Some(path.as_ref().to_path_buf());
        self
    }

    /// Resolve the unix domain socket path, taking either the override
    /// or performing discovery.
    async fn resolve_unix_domain_path(&self) -> Result<PathBuf, Error> {
        if let Some(path) = self.unix_domain.as_ref() {
            Ok(path.clone())
        } else {
            let watchman_path = self
                .watchman_cli_path
                .as_ref()
                .map_or_else(|| Path::new("watchman"), |p| p.as_ref());

            let mut cmd = Command::new(watchman_path);
            cmd.args(["--output-encoding", "bser-v2", "get-sockname"]);

            #[cfg(windows)]
            cmd.creation_flags(winapi::um::winbase::CREATE_NO_WINDOW);

            let output = cmd
                .output()
                .await
                .map_err(|source| Error::ConnectionDiscovery {
                    watchman_path: watchman_path.to_path_buf(),
                    reason: source.to_string(),
                    stderr: "".to_string(),
                })?;

            let info: GetSockNameResponse =
                serde_bser::from_slice(&output.stdout).map_err(|source| {
                    Error::ConnectionDiscovery {
                        watchman_path: watchman_path.to_path_buf(),
                        reason: source.to_string(),
                        stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
                    }
                })?;

            let debug = format!("{:#?}", info);

            if let Some(message) = info.error {
                return Err(Error::WatchmanServerError {
                    message,
                    command: "get-sockname".into(),
                });
            }

            info.sockname.ok_or_else(|| Error::MissingField {
                fieldname: "sockname",
                command: "get-sockname".into(),
                response: debug,
            })
        }
    }

    /// Establish a connection to the watchman server.
    /// If the connector was configured to perform discovery (which is
    /// the default configuration), then this will attempt to start
    /// the watchman server.
    pub async fn connect(&self) -> Result<Client, Error> {
        let sock_path = self.resolve_unix_domain_path().await?;

        #[cfg(unix)]
        let stream = UnixStream::connect(sock_path)
            .await
            .map_err(Error::ConnectionError)?;

        #[cfg(windows)]
        let stream = named_pipe::NamedPipe::connect(sock_path).await?;

        let stream: Box<dyn ReadWriteStream> = Box::new(stream);

        let (reader, writer) = tokio::io::split(stream);

        let (request_tx, request_rx) = tokio::sync::mpsc::channel(128);

        let mut task = ClientTask {
            writer,
            reader: FramedRead::new(reader, BserSplitter),
            request_rx,
            request_queue: VecDeque::new(),
            waiting_response: false,
            subscriptions: HashMap::new(),
        };
        tokio::spawn(async move {
            if let Err(err) = task.run().await {
                let _ignored = writeln!(io::stderr(), "watchman client task failed: {}", err);
            }
        });

        let inner = Arc::new(Mutex::new(ClientInner { request_tx }));

        Ok(Client { inner })
    }
}

/// Represents a canonical path in the filesystem.
#[derive(Debug, Clone)]
pub struct CanonicalPath(PathBuf);

impl CanonicalPath {
    /// Construct the canonical version of the supplied path.
    /// This function will canonicalize the path and return the
    /// result, if successful.
    /// If you have already canonicalized the path, it is preferable
    /// to use the `with_canonicalized_path` function instead.
    pub fn canonicalize<P: AsRef<Path>>(path: P) -> Result<Self, std::io::Error> {
        let path = std::fs::canonicalize(path)?;
        Ok(Self(Self::strip_unc_escape(path)))
    }

    /// Construct from an already canonicalized path.
    /// This function will panic if the supplied path is not an absolute
    /// path!
    pub fn with_canonicalized_path(path: PathBuf) -> Self {
        assert!(
            path.is_absolute(),
            "attempted to call \
             CanonicalPath::with_canonicalized_path on a non-canonical path! \
             You probably want to call CanonicalPath::canonicalize instead!"
        );
        Self(Self::strip_unc_escape(path))
    }

    /// Watchman doesn't like the UNC prefix being present for incoming paths
    /// in its current implementation: we should fix that, but in the meantime
    /// we want clients to be able to connect to existing versions, so let's
    /// strip off the UNC escape
    #[cfg(windows)]
    #[inline]
    fn strip_unc_escape(path: PathBuf) -> PathBuf {
        match path.to_str() {
            Some(s) if s.starts_with("\\\\?\\") => PathBuf::from(&s[4..]),
            _ => path,
        }
    }

    #[cfg(unix)]
    #[inline]
    fn strip_unc_escape(path: PathBuf) -> PathBuf {
        path
    }

    /// Consume self yielding the canonicalized PathBuf.
    pub fn into_path_buf(self) -> PathBuf {
        self.0
    }
}

/// Data that describes a watched filesystem location.
/// Watchman performs watch aggregation to project boundaries, so a request
/// to watch a subdirectory will resolve to the higher level root path
/// and a relative path offset.
/// This struct encodes both pieces of information.
#[derive(Debug, Clone)]
pub struct ResolvedRoot {
    root: PathBuf,
    relative: Option<PathBuf>,
    watcher: String,
}

impl ResolvedRoot {
    /// Returns the name of the watcher that the server is using to
    /// monitor the path.  The watcher is generally system dependent,
    /// but some systems offer multipler watchers.
    /// You generally don't care too much about the watcher that is
    /// in use, but if the watcher is a virtualized filesystem such as
    /// `eden` then you may wish to use to alternative queries to get the
    /// best performance.
    pub fn watcher(&self) -> &str {
        self.watcher.as_str()
    }

    /// Returns the root of the watchman project that is being watched
    pub fn project_root(&self) -> &Path {
        &self.root
    }

    /// Returns the absolute path to the directory that you requested be resolved.
    pub fn path(&self) -> PathBuf {
        if let Some(relative) = self.relative.as_ref() {
            self.root.join(relative)
        } else {
            self.root.clone()
        }
    }

    /// Returns the path to the directory that you requested be resolved,
    /// relative to the `project_root`.
    pub fn project_relative_path(&self) -> Option<&Path> {
        self.relative.as_ref().map(PathBuf::as_ref)
    }
}

trait ReadWriteStream: AsyncRead + AsyncWrite + std::marker::Unpin + Send {}

#[cfg(unix)]
impl ReadWriteStream for UnixStream {}

struct SendRequest {
    /// The serialized request to send to the server
    buf: Vec<u8>,
    /// to pass the response back to the requstor
    tx: tokio::sync::oneshot::Sender<Result<Bytes, String>>,
}

impl SendRequest {
    fn respond(self, result: Result<Bytes, String>) {
        let _ = self.tx.send(result);
    }
}

enum SubscriptionNotification {
    Pdu(Bytes),
    Canceled,
}

enum TaskItem {
    QueueRequest(SendRequest),
    RegisterSubscription(String, UnboundedSender<SubscriptionNotification>),
}

/// Splits BSER mesages out of a stream. Does not attempt to actually decode them.
struct BserSplitter;

impl Decoder for BserSplitter {
    type Item = Bytes;
    type Error = TaskError;

    fn decode(&mut self, buf: &mut BytesMut) -> Result<Option<Self::Item>, Self::Error> {
        let mut bunser = Bunser::new(SliceRead::new(buf.as_ref()));

        let pdu = match bunser.read_pdu() {
            Ok(pdu) => pdu,
            Err(source) => {
                // We know that the smallest full PDU returned by the server won't ever be smaller
                // than this size. So, if we have less than BUF_SIZE bytes, ask for more data.
                const BUF_SIZE: usize = 16;

                let missing = BUF_SIZE.saturating_sub(buf.len());

                if missing > 0 {
                    buf.reserve(missing);
                    return Ok(None);
                }

                // We should have succeded in reading some data here, but we didn't. Return an
                // error.
                return Err(TaskError::Deserialize {
                    source: source.into(),
                    data: buf.to_vec(),
                });
            }
        };

        let total_size = (pdu.start + pdu.len) as usize;

        let missing = total_size.saturating_sub(buf.len());
        if missing > 0 {
            buf.reserve(missing);
            return Ok(None);
        }

        let ret = buf.split_to(total_size);
        Ok(Some(ret.freeze()))
    }
}

/// A live connection to a watchman server.
/// Use [Connector](struct.Connector.html) to establish a connection.
pub struct Client {
    inner: Arc<Mutex<ClientInner>>,
}

/// The client task coordinates sending requests with processing
/// unilateral results
struct ClientTask {
    writer: tokio::io::WriteHalf<Box<dyn ReadWriteStream>>,
    reader: FramedRead<tokio::io::ReadHalf<Box<dyn ReadWriteStream>>, BserSplitter>,
    request_rx: Receiver<TaskItem>,
    request_queue: VecDeque<SendRequest>,
    waiting_response: bool,
    subscriptions: HashMap<String, UnboundedSender<SubscriptionNotification>>,
}

impl Drop for ClientTask {
    fn drop(&mut self) {
        self.fail_all(&TaskError::Shutdown)
    }
}

impl ClientTask {
    async fn run(&mut self) -> Result<(), TaskError> {
        // process things, and if we encounter an error, ensure that
        // we fail all outstanding requests
        match self.run_loop().await {
            Err(err) => {
                self.fail_all(&err);
                Err(err)
            }
            ok => ok,
        }
    }

    async fn run_loop(&mut self) -> Result<(), TaskError> {
        loop {
            futures::select_biased! {
                pdu = self.reader.next().fuse() => {
                    match pdu {
                        Some(pdu) => self.process_pdu(pdu?).await?,
                        None => return Err(TaskError::Eof),
                    }
                }
                task = self.request_rx.recv().fuse() => {
                    match task {
                        Some(TaskItem::QueueRequest(request)) => self.queue_request(request).await?,
                        Some(TaskItem::RegisterSubscription(name, tx)) => {
                            self.register_subscription(name, tx)
                        }
                        None => break,
                    }
                }
            }
        }

        Ok(())
    }

    fn register_subscription(
        &mut self,
        name: String,
        tx: UnboundedSender<SubscriptionNotification>,
    ) {
        self.subscriptions.insert(name, tx);
    }

    /// Generate an error for each queued request.
    /// This is called in situations where the state of the connection
    /// to the serve is non-recoverable.
    fn fail_all(&mut self, err: &TaskError) {
        while let Some(request) = self.request_queue.pop_front() {
            request.respond(Err(err.to_string()));
        }
    }

    /// If we're not waiting for the response to a request,
    /// then send the next one!
    async fn send_next_request(&mut self) -> Result<(), TaskError> {
        if !self.waiting_response && !self.request_queue.is_empty() {
            match self
                .writer
                .write_all(&self.request_queue.front().expect("not empty").buf)
                .await
            {
                Err(err) => {
                    // A failed write breaks our world; we don't want to
                    // try to continue
                    return Err(err.into());
                }
                Ok(_) => self.waiting_response = true,
            }
        }
        Ok(())
    }

    /// Queue up a new request from the client code, and then
    /// check to see if we can send a queued request to the server.
    async fn queue_request(&mut self, request: SendRequest) -> Result<(), TaskError> {
        self.request_queue.push_back(request);
        self.send_next_request().await?;
        Ok(())
    }

    /// Dispatch a PDU that we just read to the appropriate client code.
    async fn process_pdu(&mut self, pdu: Bytes) -> Result<(), TaskError> {
        use serde::Deserialize;
        #[derive(Deserialize, Debug)]
        pub struct Unilateral {
            #[allow(unused)]
            pub unilateral: bool,
            pub subscription: String,
            #[serde(default)]
            pub canceled: bool,
        }

        if let Ok(unilateral) = bunser::<Unilateral>(&pdu) {
            if let Some(subscription) = self.subscriptions.get_mut(&unilateral.subscription) {
                let msg = if unilateral.canceled {
                    SubscriptionNotification::Canceled
                } else {
                    SubscriptionNotification::Pdu(pdu)
                };

                if subscription.send(msg).is_err() || unilateral.canceled {
                    // The `Subscription` was dropped; we don't need to
                    // treat this as terminal for this client session,
                    // so just de-register the handler
                    self.subscriptions.remove(&unilateral.subscription);
                }
            }
        } else if self.waiting_response {
            let request = self
                .request_queue
                .pop_front()
                .expect("waiting_response is only true when request_queue is not empty");
            self.waiting_response = false;

            request.respond(Ok(pdu));
        } else {
            // This should never happen as we're not doing any subscription stuff
            return Err(TaskError::UnilateralPdu);
        }

        self.send_next_request().await?;
        Ok(())
    }
}

fn bunser<T>(buf: &[u8]) -> Result<T, Error>
where
    T: serde::de::DeserializeOwned,
{
    let response: T = serde_bser::from_slice(buf).map_err(|source| Error::Deserialize {
        source: source.into(),
        data: buf.to_vec(),
    })?;
    Ok(response)
}

struct ClientInner {
    request_tx: Sender<TaskItem>,
}

impl ClientInner {
    /// This method will send a request to the watchman server
    /// and wait for its response.
    /// This is really an internal method, but it is made public in case a
    /// consumer of this crate needs to issue a command for which we haven't
    /// yet made an ergonomic wrapper.
    pub(crate) async fn generic_request<Request, Response>(
        &mut self,
        request: Request,
    ) -> Result<Response, Error>
    where
        Request: serde::Serialize + std::fmt::Debug,
        Response: serde::de::DeserializeOwned,
    {
        // Step 1: serialize into a bser byte buffer
        let mut request_data = vec![];
        serde_bser::ser::serialize(&mut request_data, &request).map_err(|source| {
            Error::Serialize {
                source: source.into(),
            }
        })?;

        // Step 2: ask the client task to send it for us
        let (tx, rx) = tokio::sync::oneshot::channel();
        self.request_tx
            .send(TaskItem::QueueRequest(SendRequest {
                buf: request_data,
                tx,
            }))
            .await
            .map_err(|_| ConnectionLost::ClientTaskExited)?;

        // Step 3: wait for the client task to give us the response
        let pdu_data = rx
            .await
            .map_err(|_| ConnectionLost::ClientTaskExited)?
            .map_err(ConnectionLost::Error)?;

        // Step 4: sniff for an error response in the deserialized data
        use serde::Deserialize;
        #[derive(Deserialize, Debug)]
        struct MaybeError {
            #[serde(default)]
            error: Option<String>,
        }

        // Step 5: deserialize into the caller-desired format
        let maybe_err: MaybeError = bunser(&pdu_data)?;
        if let Some(message) = maybe_err.error {
            return Err(Error::WatchmanServerError {
                message,
                command: format!("{:#?}", request),
            });
        }

        let response: Response = bunser(&pdu_data)?;
        Ok(response)
    }
}

/// Returned by [Subscription::next](struct.Subscription.html#method.next)
/// as events are observed by Watchman.
#[allow(clippy::large_enum_variant)]
#[derive(Debug, Clone)]
pub enum SubscriptionData<F>
where
    F: serde::de::DeserializeOwned + std::fmt::Debug + Clone + QueryFieldList,
{
    /// The Subscription was canceled.
    /// This could be for a number of reasons that are not knowable
    /// to the client:
    /// * The user may have issued the `watch-del` command
    /// * The containing watch root may have been deleted or
    ///   un-mounted
    /// * The containing watch may no longer be accessible
    ///   to the watchman user/process
    /// * Some other error condition that renders the project
    ///   unwatchable may have occurred
    /// * The server may have been gracefully shutdown
    ///
    /// A Canceled subscription will deliver no further results.
    Canceled,

    /// Files matching your criteria have changed.
    /// The QueryResult contains the details.
    /// Pay attention to the
    /// [is_fresh_instance](pdu/struct.QueryResult.html#structfield.is_fresh_instance) field!
    FilesChanged(QueryResult<F>),

    /// Some other watchman client has broadcast that the watched
    /// project is entering a new named state.
    /// For example, `hg.update` may be generated by the FB
    /// internal source control system to indicate that the
    /// working copy is about to be updated to a new revision.
    /// The metadata field contains data specific to the named
    /// state.
    StateEnter {
        state_name: String,
        metadata: Option<Value>,
    },
    /// Some other watchman client has broadcast that the watched
    /// project is no longer in the named state.
    /// This event can also be generated if the watchman client
    /// that entered the state disconnects unexpectedly from
    /// the watchman server.
    /// The `metadata` field will be `None` in that situation.
    StateLeave {
        state_name: String,
        metadata: Option<Value>,
    },
}

/// A handle to a subscription initiated via `Client::subscribe`.
/// Repeatedly call `Subscription::next().await` to yield the next
/// set of subscription results.
/// Use the `cancel` method to gracefully halt this subscription
/// if you have a program that creates and destroys subscriptions
/// throughout its lifetime.
pub struct Subscription<F>
where
    F: serde::de::DeserializeOwned + std::fmt::Debug + Clone + QueryFieldList,
{
    name: String,
    inner: Arc<Mutex<ClientInner>>,
    root: ResolvedRoot,
    responses: UnboundedReceiver<SubscriptionNotification>,
    _phantom: PhantomData<F>,
}

impl<F> Subscription<F>
where
    F: serde::de::DeserializeOwned + std::fmt::Debug + Clone + QueryFieldList,
{
    /// Returns the assigned name for this subscription instance.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Yield the next set of subscription data.
    /// An error is generated if the subscription is disconnected
    /// from the server.
    #[allow(clippy::should_implement_trait)]
    pub async fn next(&mut self) -> Result<SubscriptionData<F>, Error> {
        let msg = self
            .responses
            .recv()
            .await
            .ok_or(ConnectionLost::ClientTaskExited)?;

        match msg {
            SubscriptionNotification::Pdu(pdu) => {
                let response: QueryResult<F> = bunser(&pdu)?;

                if let Some(state_name) = response.state_enter {
                    Ok(SubscriptionData::StateEnter {
                        state_name,
                        metadata: response.state_metadata,
                    })
                } else if let Some(state_name) = response.state_leave {
                    Ok(SubscriptionData::StateLeave {
                        state_name,
                        metadata: response.state_metadata,
                    })
                } else {
                    Ok(SubscriptionData::FilesChanged(response))
                }
            }
            SubscriptionNotification::Canceled => {
                self.responses.close();
                Ok(SubscriptionData::Canceled)
            }
        }
    }

    /// Gracefully cancel this subscription.
    /// If you are imminently about to drop the associated client then you
    /// need not call this method.
    /// However, if the associated client is going to live much longer
    /// than a Subscription that you are about to drop,
    /// then it is recommended that you call `cancel` so that the server
    /// will stop delivering data about it.
    pub async fn cancel(self) -> Result<(), Error> {
        let mut inner = self.inner.lock().await;
        let _: UnsubscribeResponse = inner
            .generic_request(Unsubscribe("unsubscribe", self.root.root, self.name))
            .await?;
        Ok(())
    }
}

impl Client {
    /// This method will send a request to the watchman server
    /// and wait for its response.
    /// This is really an internal method, but it is made public in case a
    /// consumer of this crate needs to issue a command for which we haven't
    /// yet made an ergonomic wrapper.
    #[doc(hidden)]
    pub async fn generic_request<Request, Response>(
        &self,
        request: Request,
    ) -> Result<Response, Error>
    where
        Request: serde::Serialize + std::fmt::Debug,
        Response: serde::de::DeserializeOwned,
    {
        let mut inner = self.inner.lock().await;
        let response: Response = inner.generic_request(request).await?;
        Ok(response)
    }

    pub async fn version(&self) -> Result<GetVersionResponse, Error> {
        self.generic_request(&["version"]).await
    }

    pub async fn watch_list(&self) -> Result<WatchListResponse, Error> {
        self.generic_request(&["watch-list"]).await
    }

    /// This method will attempt to assert the state named `state_name`
    /// on the watchman server. This is used to facilitate advanced settling
    /// in subscriptions.
    ///
    /// Only one client can assert a given named state for a given root at
    /// a given time; an error will be returned if another client owns the
    /// requested state assertion.
    ///
    /// If successful, the state will remain asserted until the owning client
    /// either issues a `state-leave` or disconnects from the server.
    ///
    /// The optional `metadata` will be published to all subscribers of the
    /// root and made visible via `SubscriptionData::StateEnter::metadata`.
    ///
    /// See also: <https://meta.github.io/watchman/docs/cmd/state-enter.html>
    pub async fn state_enter(
        &self,
        root: &ResolvedRoot,
        state_name: &str,
        sync_timeout: SyncTimeout,
        metadata: Option<Value>,
    ) -> Result<(), Error> {
        let request = StateEnterLeaveRequest(
            "state-enter",
            root.root.clone(),
            StateEnterLeaveParams {
                name: state_name,
                metadata,
                sync_timeout,
            },
        );

        let _response: StateEnterLeaveResponse = self.generic_request(request).await?;
        Ok(())
    }

    /// This method will attempt to release an owned state assertion for the
    /// state named `state_name` on the watchman server. This is used to facilitate
    /// advanced settling in subscriptions.
    ///
    /// The optional `metadata` will be published to all subscribers of the
    /// root and made visible via `SubscriptionData::StateLeave::metadata`.
    ///
    /// See also: <https://meta.github.io/watchman/docs/cmd/state-leave.html>
    pub async fn state_leave(
        &self,
        root: &ResolvedRoot,
        state_name: &str,
        sync_timeout: SyncTimeout,
        metadata: Option<Value>,
    ) -> Result<(), Error> {
        let request = StateEnterLeaveRequest(
            "state-leave",
            root.root.clone(),
            StateEnterLeaveParams {
                name: state_name,
                metadata,
                sync_timeout,
            },
        );

        let _response: StateEnterLeaveResponse = self.generic_request(request).await?;
        Ok(())
    }

    /// This is typically the first method invoked on a client.
    /// Its purpose is to ensure that the watchman server is watching the specified
    /// path and to resolve it to a `ResolvedRoot` instance.
    ///
    /// The path to resolve must be a canonical path; watchman performs strict name
    /// resolution to detect TOCTOU issues and will generate an error if the path
    /// is not the canonical name.
    ///
    /// Note that for regular filesystem watches, if the requested path is not
    /// yet being watched, this method will not yield until the watchman server
    /// has completed a recursive crawl of that portion of the filesystem.
    /// In other words, the worst case performance of this is
    /// `O(recursive-number-of-files)` and is impacted by the underlying storage
    /// device and its performance characteristics.
    pub async fn resolve_root(&self, path: CanonicalPath) -> Result<ResolvedRoot, Error> {
        let response: WatchProjectResponse = self
            .generic_request(WatchProjectRequest("watch-project", path.0.clone()))
            .await?;

        Ok(ResolvedRoot {
            root: response.watch,
            relative: response.relative_path,
            watcher: response.watcher,
        })
    }

    /// Perform a generic watchman query.
    /// The `F` type is a struct defined by the
    /// [query_result_type!](macro.query_result_type.html) macro,
    /// or, if you want only the file name from the results, the
    /// [NameOnly](struct.NameOnly.html) struct.
    ///
    /// ```
    /// use serde::Deserialize;
    /// use watchman_client::prelude::*;
    ///
    /// query_result_type! {
    ///     struct NameAndType {
    ///         name: NameField,
    ///         file_type: FileTypeField,
    ///     }
    /// }
    ///
    /// async fn query(
    ///     client: &mut Client,
    ///     resolved: &ResolvedRoot,
    /// ) -> Result<(), Box<dyn std::error::Error>> {
    ///     let response: QueryResult<NameAndType> = client
    ///         .query(
    ///             &resolved,
    ///             QueryRequestCommon {
    ///                 glob: Some(vec!["**/*.rs".to_string()]),
    ///                 ..Default::default()
    ///             },
    ///         )
    ///         .await?;
    ///     println!("response: {:#?}", response);
    ///     Ok(())
    /// }
    /// ```
    ///
    /// When constructing your result type, you can select from the
    /// following fields:
    ///
    /// * [CTimeAsFloatField](struct.CTimeAsFloatField.html)
    /// * [CTimeField](struct.CTimeField.html)
    /// * [ContentSha1HexField](struct.ContentSha1HexField.html)
    /// * [CreatedClockField](struct.CreatedClockField.html)
    /// * [DeviceNumberField](struct.DeviceNumberField.html)
    /// * [ExistsField](struct.ExistsField.html)
    /// * [FileTypeField](struct.FileTypeField.html)
    /// * [InodeNumberField](struct.InodeNumberField.html)
    /// * [MTimeAsFloatField](struct.MTimeAsFloatField.html)
    /// * [MTimeField](struct.MTimeField.html)
    /// * [ModeAndPermissionsField](struct.ModeAndPermissionsField.html)
    /// * [NameField](struct.NameField.html)
    /// * [NewField](struct.NewField.html)
    /// * [NumberOfLinksField](struct.NumberOfLinksField.html)
    /// * [ObservedClockField](struct.ObservedClockField.html)
    /// * [OwnerGidField](struct.OwnerGidField.html)
    /// * [OwnerUidField](struct.OwnerUidField.html)
    /// * [SizeField](struct.SizeField.html)
    /// * [SymlinkTargetField](struct.SymlinkTargetField.html)
    ///
    /// (See [the fields module](fields/index.html) for a definitive list)
    ///
    /// The file names are all relative to the `root` parameter.
    pub async fn query<F>(
        &self,
        root: &ResolvedRoot,
        query: QueryRequestCommon,
    ) -> Result<QueryResult<F>, Error>
    where
        F: serde::de::DeserializeOwned + std::fmt::Debug + Clone + QueryFieldList,
    {
        let query = QueryRequest(
            "query",
            root.root.clone(),
            QueryRequestCommon {
                relative_root: root.relative.clone(),
                fields: F::field_list(),
                ..query
            },
        );

        let response: QueryResult<F> = self.generic_request(query.clone()).await?;

        Ok(response)
    }

    /// Create a Subscription that will yield file changes as they occur in
    /// real time.
    /// The `F` type is a struct defined by the
    /// [query_result_type!](macro.query_result_type.html) macro,
    /// or, if you want only the file name from the results, the
    /// [NameOnly](struct.NameOnly.html) struct.
    ///
    /// Returns two pieces of information:
    /// * A [Subscription](struct.Subscription.html) handle that can be used to yield changes
    ///   as they are observed by watchman
    /// * A [SubscribeResponse](pdu/struct.SubscribeResponse.html) that contains some data about the
    ///   state of the watch at the time the subscription was
    ///   initiated
    pub async fn subscribe<F>(
        &self,
        root: &ResolvedRoot,
        query: SubscribeRequest,
    ) -> Result<(Subscription<F>, SubscribeResponse), Error>
    where
        F: serde::de::DeserializeOwned + std::fmt::Debug + Clone + QueryFieldList,
    {
        let name = format!(
            "sub-[{}]-{}",
            std::env::args()
                .next()
                .unwrap_or_else(|| "<no-argv-0>".to_string()),
            SUB_ID.fetch_add(1, Ordering::Relaxed)
        );

        let query = SubscribeCommand(
            "subscribe",
            root.root.clone(),
            name.clone(),
            SubscribeRequest {
                relative_root: root.relative.clone(),
                fields: F::field_list(),
                ..query
            },
        );

        let (tx, responses) = tokio::sync::mpsc::unbounded_channel();

        {
            let inner = self.inner.lock().await;
            inner
                .request_tx
                .send(TaskItem::RegisterSubscription(name.clone(), tx))
                .await
                .map_err(|_| ConnectionLost::ClientTaskExited)?;
        }

        let subscription = Subscription::<F> {
            name,
            inner: Arc::clone(&self.inner),
            root: root.clone(),
            responses,
            _phantom: PhantomData,
        };

        let response: SubscribeResponse = self.generic_request(query).await?;

        Ok((subscription, response))
    }

    /// Expand a set of globs into the set of matching file names.
    /// The globs must be relative to the `root` parameter.
    /// The returned file names are all relative to the `root` parameter.
    pub async fn glob(&self, root: &ResolvedRoot, globs: &[&str]) -> Result<Vec<PathBuf>, Error> {
        let response: QueryResult<NameOnly> = self
            .query(
                root,
                QueryRequestCommon {
                    relative_root: root.relative.clone(),
                    glob: Some(globs.iter().map(|&s| s.to_string()).collect()),
                    ..Default::default()
                },
            )
            .await?;
        Ok(response
            .files
            .unwrap_or_else(Vec::new)
            .into_iter()
            .map(|f| f.name.into_inner())
            .collect())
    }

    /// Returns the current clock value for a watched root.
    /// If `sync_timeout` is `SyncTimeout::DisableCookie` then the instantaneous
    /// clock value is returned without using a sync cookie.
    ///
    /// Otherwise, a sync cookie will be created and the server will wait
    /// for up to the associated `sync_timeout` duration to observe it.
    /// If that timeout is reached, this method will yield an error.
    ///
    /// When should you use a cookie?  If you need to a clock value that is
    /// guaranteed to reflect any filesystem changes that happened before
    /// a given point in time you should use a sync cookie.
    ///
    /// ## See also:
    ///  * <https://meta.github.io/watchman/docs/cmd/clock.html>
    ///  * <https://meta.github.io/watchman/docs/cookies.html>
    pub async fn clock(
        &self,
        root: &ResolvedRoot,
        sync_timeout: SyncTimeout,
    ) -> Result<ClockSpec, Error> {
        let response: ClockResponse = self
            .generic_request(ClockRequest(
                "clock",
                root.root.clone(),
                ClockRequestParams { sync_timeout },
            ))
            .await?;
        Ok(response.clock)
    }

    /// Returns the current configuration for a watched root.
    pub async fn get_config(&self, root: &ResolvedRoot) -> Result<WatchmanConfig, Error> {
        let response: GetConfigResponse = self
            .generic_request(GetConfigRequest("get-config", root.root.clone()))
            .await?;
        Ok(response.config)
    }

    /// Registers a trigger.
    pub async fn register_trigger(
        &self,
        root: &ResolvedRoot,
        request: TriggerRequest,
    ) -> Result<TriggerResponse, Error> {
        let response: TriggerResponse = self
            .generic_request(TriggerCommand("trigger", root.root.clone(), request))
            .await?;
        Ok(response)
    }

    /// Removes a registered trigger.
    pub async fn remove_trigger(
        &self,
        root: &ResolvedRoot,
        name: &str,
    ) -> Result<TriggerDelResponse, Error> {
        let response: TriggerDelResponse = self
            .generic_request(TriggerDelCommand(
                "trigger-del",
                root.root.clone(),
                name.into(),
            ))
            .await?;
        Ok(response)
    }

    /// Lists registered triggers.
    pub async fn list_triggers(&self, root: &ResolvedRoot) -> Result<TriggerListResponse, Error> {
        let response: TriggerListResponse = self
            .generic_request(TriggerListCommand("trigger-list", root.root.clone()))
            .await?;
        Ok(response)
    }

    pub async fn get_asserted_states(
        &self,
        root: &ResolvedRoot,
    ) -> Result<GetAssertedStatesResponse, Error> {
        let request = GetAssertedStatesRequest("debug-get-asserted-states", root.root.clone());

        let response: GetAssertedStatesResponse = self.generic_request(request).await?;
        Ok(response)
    }
}

#[cfg(test)]
mod tests {
    use std::io;

    use futures::stream;
    use futures::stream::TryStreamExt;
    use serde::Deserialize;
    use serde::Serialize;
    use tokio_util::io::StreamReader;

    use super::*;

    #[derive(Serialize, Deserialize, PartialEq, Debug)]
    struct TestStruct {
        value: i32,
    }

    #[test]
    fn connection_builder_paths() {
        let builder = Connector::new().unix_domain_socket("/some/path");
        assert_eq!(builder.unix_domain, Some(PathBuf::from("/some/path")));
    }

    #[tokio::test]
    async fn test_decoder() {
        async fn read_bser(buf: &[u8], chunk_size: usize) -> Vec<TestStruct> {
            let chunks = buf
                .chunks(chunk_size)
                .map(|c| Result::<_, io::Error>::Ok(Bytes::copy_from_slice(c)));

            let reader = StreamReader::new(stream::iter(chunks));

            FramedRead::new(reader, BserSplitter)
                .map_err(TaskError::from)
                .and_then(|bytes| async move {
                    // We unwrap this since a) this is a test and b) serde_bser's errors aren't
                    // easily propagated into en error type like anyhow::Error without losing the
                    // message.
                    Ok(serde_bser::from_slice::<TestStruct>(&bytes).unwrap())
                })
                .try_collect()
                .await
                .unwrap()
        }

        let msgs = vec![
            TestStruct { value: 1 },
            TestStruct { value: 2 },
            TestStruct { value: 3 },
        ];

        let mut buf = vec![];

        for msg in msgs.iter() {
            serde_bser::ser::serialize(&mut buf, msg).expect("Failed to write to a Vec");
        }

        // Read it with various sizes
        assert_eq!(msgs, read_bser(&buf, 1).await);
        assert_eq!(msgs, read_bser(&buf, 2).await);
        assert_eq!(msgs, read_bser(&buf, 10).await);
        assert_eq!(msgs, read_bser(&buf, buf.len()).await);
    }

    #[test]
    fn test_decoder_err() {
        let mut bytes = BytesMut::new();

        // We don't error if there isn't much data yet
        bytes.extend_from_slice(&[0; 10]);
        let r1 = BserSplitter.decode(&mut bytes);
        assert!(r1.is_ok());
        assert!(r1.unwrap().is_none());

        // We do if there is enough
        bytes.extend_from_slice(&[0; 10]);
        let r1 = BserSplitter.decode(&mut bytes);
        assert!(r1.is_err());
    }

    #[test]
    fn test_bounds() {
        fn assert_bounds<T: std::error::Error + Sync + Send + 'static>() {}
        assert_bounds::<Error>();
        assert_bounds::<TaskError>();
    }
}
