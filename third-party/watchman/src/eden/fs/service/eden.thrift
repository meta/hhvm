/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

include "eden/fs/config/eden_config.thrift"
include "fb303/thrift/fb303_core.thrift"
include "thrift/annotation/cpp.thrift"
include "thrift/annotation/rust.thrift"
include "thrift/annotation/thrift.thrift"

cpp_include "eden/fs/utils/GlobPath.h"

@thrift.AllowLegacyMissingUris
package;

namespace cpp2 facebook.eden
namespace java com.facebook.eden.thrift
namespace java.swift com.facebook.eden.thrift
namespace py facebook.eden
namespace py3 eden.fs.service
namespace hack edenfs.service

/**
 * API style guide.
 * ----------------
 *
 * These guides are to ensure we use consistent practices to make
 * our interface easy to use.
 * 1. Wrap the endpoint arguments in a struct. The name of this argument
 * struct should be the endpointname + "Request". This is Thrift's recommended
 * practice for arguments and ensures that we can safely evolve the arguments
 * of a method.
 * 2. Wrap the return value from the endpoint in a struct, even if it is just
 * a single value. This allows evolving return types without having to create a
 * whole new endpoint. The name of this return value struct should be
 * endpointname + "Response".
 * 3. If your endpoint operates on a mount(s), make the mount identifier
 * the first value in your arguments struct. Use the MountId struct as the type.
 * This allows us to evolve the identifiers we use for mountpoints in the future
 * without rewriting your new endpoint.
 */

/**
 * To support testing of the EdenFS client library, we need to be able to
 * mock the EdenFS Thrift APIs. This is done by defining a mock service,
 * using `mockall::mock!`, that implements the EdenFS Thrift APIs.
 *
 * The mock definitions will require updates whenever the EdenFS Thrift API
 * changes. Not all changes will require an update, only those that either:
 *   1. Add/Rename a new method to the interface
 *   2. Change the parameters or return type of a method
 *   3. Introduce a breaking Thrift API changes (e.g. rename a method)
 *
 * For reference, the EdenFS client library mocks are defined here:
 *   https://www.internalfb.com/code/fbsource/fbcode/eden/fs/cli_rs/edenfs-client/src/client/mock_service.rs
 */

/** Thrift doesn't really do unsigned numbers, but we can sort of fake it.
 * This type is serialized as an integer value that is 64-bits wide and
 * should round-trip with full fidelity for C++ client/server, but for
 * other runtimes will have crazy results if the sign bit is ever set.
 * In practice it is impossible for us to have files that large in eden,
 * and sequence numbers will take an incredibly long time to ever roll
 * over and cause problems.
 * Once t13345978 is done, we can uncomment the cpp.type below.
 */
typedef i64 /* (cpp.type = "std::uint64_t") */
unsigned64

typedef i32 pid_t

/**
 * A backing-store-specific identifier for the root tree. For Mercurial or
 * Git, this is a 20-byte binary hash or a 40-byte hexadecimal hash.
 *
 * For other backing stores, this string may have variable length and its
 * meaning is defined by the backing-store. If possible, prefer human-readable
 * strings so they can be read in log files and error messages.
 *
 * This is named ThriftRootId to not conflict with the RootId class, but
 * perhaps this Thrift module should be placed in its own namespace.
 */
typedef binary ThriftRootId

typedef binary ThriftObjectId

/**
 * A source control hash.
 *
 * This should normally be a 20-byte binary value, however the edenfs server
 * will accept BinaryHash arguments as 40-byte hexadecimal strings as well.
 * Data returned by the edenfs server in a BinaryHash field will always be a
 * 20-byte binary value.
 */
typedef binary BinaryHash

/**
 * So, you thought that a path was a string?
 * Paths in posix are arbitrary byte strings with some pre-defined special
 * characters.  On modern systems they tend to be represented as UTF-8 but
 * there is no guarantee.  We use the `PathString` type as symbolic way
 * to indicate that you may need to perform special processing to safely
 * interpret the path data on your system.
 */
typedef binary PathString

@cpp.Adapter{name = "::facebook::eden::GlobPathAdapter"}
typedef PathString GlobPathValue

/**
 * Bit set indicating where data should be fetched from in our debugging
 * commands.
 * Bits are defined by DataFetchOriginSet.
 */
typedef unsigned64 DataFetchOriginSet

/**
 * A customizable type to be returned with an EdenError, helpful for catching
 * and having custom client logic to handle specific error cases
 */
enum EdenErrorType {
  /** The errorCode property is a posix errno value */
  POSIX_ERROR = 0,
  /** The errorCode property is a win32 error value */
  WIN32_ERROR = 1,
  /** The errorCode property is a windows NT HResult error value */
  HRESULT_ERROR = 2,
  /**
   * An argument passed to thrift was invalid. errorCode will be set to EINVAL
   */
  ARGUMENT_ERROR = 3,
  /** An error occurred. errorCode will be not set */
  GENERIC_ERROR = 4,
  /** The mount generation changed. errorCode will be set to ERANGE */
  MOUNT_GENERATION_CHANGED = 5,
  /** The journal has been truncated. errorCode will be set to EDOM */
  JOURNAL_TRUNCATED = 6,
  /**
   * The thrift function that receives this in an error is being called while
   * a checkout is in progress. errorCode will not be set.
   */
  CHECKOUT_IN_PROGRESS = 7,
  /**
  * The thrift function that receives this is an error is being called with a
  * parent that is not the current parent. errorCode will not be set.
  */
  OUT_OF_DATE_PARENT = 8,
  /**
  * The requested attribute is not available for the given file. errorCode will
  * be set to ENOENT.
  */
  ATTRIBUTE_UNAVAILABLE = 9,
  /**
   * An error occurred during request cancellation. The errorCode field will
   * contain specific details about the cancellation failure.
   */
  CANCELLATION_ERROR = 10,
  /**
  * The errorCode property is a network error code.
  *
  * If there should be a code collision with different networking libraries,
  * the higher bits of the error code can be utilized to disambiguate.
  */
  NETWORK_ERROR = 11,
  /**
   * The current EdenFS Thrift service is intentionally shutting down, so the
   * request or stream cannot continue. Clients should reconnect to the
   * recovered or replacement service. errorCode will not be set.
   */
  SHUTTING_DOWN = 12,
}

exception EdenError {
  @thrift.ExceptionMessage
  1: string message;
  2: optional i32 errorCode;
  3: EdenErrorType errorType;
}

exception NoValueForKeyError {
  1: string key;
}

struct MountId {
  1: PathString mountPoint;
}

/**
 * Information about the running edenfs daemon.
 */
struct DaemonInfo {
  1: i32 pid;
  /**
   * List of command line arguments, including the executable name,
   * given to the edenfs process.
   */
  2: list<string> commandLine;
  /**
   * The service status.
   *
   * This is almost the same value reported by
   * fb303_core.BaseService.getStatus(). fb303_core.BaseService.getStatus()
   * only returns the Thrift server status and does not understand mounts or
   * graceful restarts.
   */
  3: optional fb303_core.fb303_status status;
  /**
   * The uptime of the edenfs daemon
   * Same data from /proc/pid/stat
   */
  4: optional float uptime;
}

/**
* Information about the privhelper process
*/
struct PrivHelperInfo {
  1: bool connected;
  2: pid_t pid;
}

/**
 * The current running state of an EdenMount.
 */
@cpp.EnumType{type = cpp.EnumUnderlyingType.U32}
enum MountState {
  /**
   * The EdenMount object has been constructed but has not started
   * initializing.
   */
  UNINITIALIZED = 0,
  /**
   * The mount point is currently initializing and loading necessary state
   * (such as the root directory contents) before it can ask the kernel to
   * mount it.
   */
  INITIALIZING = 1,
  /**
   * The mount point has loaded its local state needed to start mounting
   * but has not actually started mounting yet.
   */
  INITIALIZED = 2,
  /**
   * Starting to mount the filesystem.
   */
  STARTING = 3,
  /**
   * The EdenMount is running normally.
   */
  RUNNING = 4,
  /**
   * Encountered an error while starting the user-space filesystem mount.
   * FUSE is a misnomer: this error state also applies to NFS and Windows'
   * Projected File System.
   * TODO: rename to INITIALIZATION_ERROR.
   */
  FUSE_ERROR = 5,
  /**
   * EdenMount::shutdown() has been called, but it is not complete yet.
   */
  SHUTTING_DOWN = 6,
  /**
   * EdenMount::shutdown() has completed, but there are still outstanding
   * references so EdenMount::destroy() has not been called yet.
   *
   * When EdenMount::destroy() is called the object can be destroyed
   * immediately.
   */
  SHUT_DOWN = 7,
  /**
   * EdenMount::destroy() has been called, but the shutdown is not complete
   * yet.  There are no remaining references to the EdenMount at this point,
   * so when the shutdown completes it will be automatically destroyed.
   */
  DESTROYING = 8,
  /**
   * An error occurred during mount initialization.
   *
   * This state is used for errors that occur during the INITIALIZING phase,
   * before we have attempted to start the user-space filesystem mount.
   */
  INIT_ERROR = 9,
}

struct MountInfo {
  1: PathString mountPoint;
  2: PathString edenClientPath;
  3: MountState state;
  4: optional PathString backingRepoPath;
  // The filesystem channel type: "fuse", "nfs3", or "prjfs".
  5: optional string fsChannelType;
  // The FUSE transport type: "devfuse" or "io_uring". Only set for FUSE mounts.
  6: optional string fuseTransport;
  // Whether the mount point is visible in the daemon's current mount namespace.
  7: optional bool visibleInDaemonNamespace;
  // The NFS transport: "tcp" or "unix". Only set for NFS mounts.
  8: optional string nfsTransport;
}

struct MountArgument {
  1: PathString mountPoint;
  2: PathString edenClientPath;
  3: bool readOnly;
}

struct UnmountArgument {
  1: MountId mountId;
  2: bool useForce = true;
}

union SHA1Result {
  1: BinaryHash sha1;
  2: EdenError error;
}

union Blake3Result {
  1: BinaryHash blake3;
  2: EdenError error;
}

union DigestHashResult {
  1: BinaryHash digestHash;
  2: EdenError error;
}

/**
 * Effectively a `struct timespec`
 */
struct TimeSpec {
  1: i64 seconds;
  2: i64 nanoSeconds;
}

/**
 * Information about filesystem entries that can be retrieved solely
 * from the tree structure, without having to fetch the actual child
 * objects from source control.
 */
struct EntryInformation {
  1: Dtype dtype;
}

union EntryInformationOrError {
  1: EntryInformation info;
  2: EdenError error;
}

/**
 * Subset of stat() data returned from getFileInformation())
 */
struct FileInformation {
  1: unsigned64 size; // wish thrift had unsigned numbers
  2: TimeSpec mtime;
  3: i32 mode; // mode_t
}

/** Holds information about a file, or an error in retrieving that info.
 * The most likely error will be ENOENT, implying that the file doesn't exist.
 */
union FileInformationOrError {
  1: FileInformation info;
  2: EdenError error;
}

/**
 * File Attributes that can be requested with getAttributesFromFilesV2(). All attributes
 * should be a power of 2. OR the requested attributes together to get a bitmask.
 */
enum FileAttributes {
  NONE = 0,
  /**
   * Returns the SHA-1 hash of a file. Returns an error for symlinks and directories,
   * and non-regular files.
   */
  SHA1_HASH = 1,
  /**
   * Returns the size of a file. Returns an error for symlinks and directories.
   * See DIGEST_SIZE if you would like to request the size of a file/directory
   * that's stored in a Content Addressed Store (i.e. RE CAS).
   */
  FILE_SIZE = 2,
  /**
   * Returns the type of a file or directory if it has a corresponding "source
   * control type" that can be represented in a source control type.
   */
  SOURCE_CONTROL_TYPE = 4,
  /**
   * Returns an opaque identifier that can be used to know when a file or directory
   * (recursively) has changed.
   *
   * If the file or directory (recursively) has been locally written to, no object ID
   * will be returned. In that case, the caller should physically compare or search
   * the directory structure.
   *
   * Do not attempt to parse this identifier. It is subject to change at any moment.
   */
  OBJECT_ID = 8,

  /**
   * Returns the BLAKE3 hash of a file. Returns an error for
   * symlinks, directories, and non-regular files. Note: the digest_hash can be
   * requested for directories as an alternative to blake3_hash.
   */
  BLAKE3_HASH = 16,

  /**
   * Returns the digest size of a given file or directory. This can be used
   * together with DIGEST_HASH to determine the key that should be used to
   * fetch a given file/directory from Content Addressed Stores (i.e. RE CAS).
   * For directories, the size of the augmented manifest that represents the
   * the directory is returned. For files, this field is the same as FILE_SIZE.
   * Returns an error for any non-directory/non-file types (symlink, exe, etc).
   */
  DIGEST_SIZE = 32,

  /**
   * Returns the digest hash of a given file or directory. This can be used
   * together with DIGEST_SIZE to determine the key that should be used to
   * fetch a given file/directory from Content Addressed Stores (i.e. RE CAS).
   * For files, this hash is just the blake3 hash of the given file. For
   * directories, this hash is blake3 hash of all the directory's descendents.
   */
  DIGEST_HASH = 64,

  /**
   * Returns the mtime of a file.
   */
  MTIME = 128,

  /**
   * Returns the mode of a file.
   */
  MODE = 256,

  /**
   * Returns whether any ACL applies to this path. Resolved from tree
   * metadata — only tree fetches are needed, avoiding the separate
   * access-check endpoint. Tree fetches may still require network
   * calls if the path is not yet cached. For directories, indicates
   * the directory has an ACL at or above it. For files and symlinks,
   * indicates the containing directory is under an ACL.
   */
  UNDER_ACL = 512,

  /**
   * Returns rich access control metadata for this path, including the
   * restriction root, ACL region names, and the groups to request
   * access through. Requires hitting a separate endpoint for ACL
   * metadata; use UNDER_ACL for the cheaper tree-only check.
   */
  ACLs = 1024,
/* NEXT_ATTR = 2^x */
}

typedef unsigned64 RequestedAttributes

/**
 * Indicates whether getAttributesForFiles requests should include results for
 * files, trees, or both.
 */
enum AttributesRequestScope {
  TREES = 0,
  FILES = 1,
  TREES_AND_FILES = 2,
}

/*
 * Sha1 hash of a file. Errors are returned when this attribute is requested for:
 *
 * 1. Symlinks: POSIX_ERROR, EINVAL
 * 2. Directories: POSIX_ERROR, EISDIR
 * 3. Non-source-control file types (FIFO, socket, char, block, whiteout, etc.): POSIX_ERROR, EINVAL
 * 4. Non-existent files: POSIX_ERROR, ENOENT
 */
union Sha1OrError {
  1: BinaryHash sha1;
  2: EdenError error;
}

/*
 * Blake3 hash of a file. Errors are returned when this attribute is requested for:
 *
 * 1. Symlinks: POSIX_ERROR, EINVAL
 * 2. Directories: POSIX_ERROR, EISDIR
 * 3. Non-source-control file types (FIFO, socket, char, block, whiteout, etc.): POSIX_ERROR, EINVAL
 * 4. Non-existent files: POSIX_ERROR, ENOENT
 * 5. Files that lack blake3 hashes: ATTRIBUTES_UNAVAILABLE, ENOENT
 */
union Blake3OrError {
  1: BinaryHash blake3;
  2: EdenError error;
}

/*
 * Size of a file. Errors are returned when this attribute is requested for:
 *
 * 1. Symlinks: POSIX_ERROR, EINVAL
 * 2. Directories: POSIX_ERROR, EISDIR
 * 3. Non-source-control file types (FIFO, socket, char, block, whiteout, etc.): POSIX_ERROR, EINVAL
 * 5. Non-existent files: POSIX_ERROR, ENOENT
 */
union SizeOrError {
  1: i64 size;
  2: EdenError error;
}

/*
 * Type that source control assigns to a given file. Errors are returned when this attribute is requested for:
 *
 * 1. Non-existent files: POSIX_ERROR, ENOENT
 *
 * In addition, this attribute may be nullopt if the file has no source control type.
 */
union SourceControlTypeOrError {
  1: SourceControlType sourceControlType;
  2: EdenError error;
}

/*
 * Object Id of a file. Errors are returned when this attribute is requested for:
 *
 * 1. Non-existent files: POSIX_ERROR, ENOENT
 *
 * If the path has been locally written to, it will have no object ID. Therefore,
 * it's possible for `objectId` to be unset even if there is no error.
 *
 * Notably, if path refers to a directory, no object ID will be returned if any
 * child file or directory has been written to.
 */
union ObjectIdOrError {
  1: ThriftObjectId objectId;
  2: EdenError error;
}

/*
 * Digest (content) size of a file or directory. Errors are returned when this attribute is requested for:
 *
 * 1. Symlinks: POSIX_ERROR, EINVAL
 * 2. Non-source-control file types (FIFO, socket, char, block, whiteout, etc.): POSIX_ERROR, EINVAL
 * 3. Non-existent files: POSIX_ERROR, ENOENT
 * 4. Files that lack digest sizes: ATTRIBUTES_UNAVAILABLE, ENOENT
 * 5. Materialized (locally modified) directories: ATTRIBUTES_UNAVAILABLE, ENOENT
 */
union DigestSizeOrError {
  1: i64 digestSize;
  2: EdenError error;
}

/*
 * Digest (content) hash of a file or directory. Errors are returned when this attribute is requested for:
 *
 * 1. Symlinks: POSIX_ERROR, EINVAL
 * 2. Non-source-control file types (FIFO, socket, char, block, whiteout, etc.): POSIX_ERROR, EINVAL
 * 3. Non-existent files: POSIX_ERROR, ENOENT
 * 4. Files/dirs that lack digest hashes: ATTRIBUTES_UNAVAILABLE, ENOENT
 * 5. Materialized (locally modified) directories: ATTRIBUTES_UNAVAILABLE, ENOENT
 */
union DigestHashOrError {
  1: BinaryHash digestHash;
  2: EdenError error;
}

/*
 * Last modified time of a file or directory. Errors are returned when this attribute is requested for:
 *
 * 1. Non-existent files: POSIX_ERROR, ENOENT
 */
union MtimeOrError {
  1: TimeSpec mtime;
  2: EdenError error;
}

/*
 * Mode bits for a file or directory. Errors are returned when this attribute is requested for:
 *
 * 1. Non-existent files: POSIX_ERROR, ENOENT
 */
union ModeOrError {
  1: i32 mode;
  2: EdenError error;
}

/**
 * Whether any ACL applies to a path, or information about an error
 * encountered when checking.
 *
 * Errors:
 * 1. Attribute not populated: ATTRIBUTE_UNAVAILABLE, ENOENT
 * 2. Tree fetch failure: GENERIC_ERROR
 */
union UnderAclOrError {
  1: bool underAcl;
  2: EdenError error;
}

/**
 * A single ACL entry governing a path. Each entry is self-describing:
 * it carries the path where the ACL is defined, the region identifier,
 * and optionally the group for requesting access.
 */
struct AclEntry {
  /**
   * The ancestor path (or the path itself) where this ACL restriction
   * is defined.
   */
  1: string restrictionRoot;

  /** The ACL region identifier (e.g., a repo region ACL string). */
  2: string repoRegionAcl;

  /**
   * The group through which access can be requested. Callers can use
   * this to render "Request Access" links. Absent if no request
   * mechanism exists for this ACL.
   */
  3: optional string requestAcl;
}

/**
 * Rich access control metadata for a path. Returned when ACLs (1024)
 * is requested.
 */
struct AclInfo {
  /**
   * Whether this path is under any ACL restriction. Included so callers
   * requesting only ACLs don't need to also request UNDER_ACL.
   * Always equals !acls.empty() when this attribute is successfully
   * resolved.
   */
  1: bool underAcl;

  /**
   * The ACL entries governing this path, ordered from nearest to
   * farthest ancestor. A path can be under multiple nested ACLs.
   * Empty list if not restricted.
   */
  2: list<AclEntry> acls;
}

/**
 * Access control metadata or information about an error encountered
 * when fetching it.
 *
 * Errors:
 * 1. Attribute not populated: ATTRIBUTE_UNAVAILABLE, ENOENT
 * 2. Network failure: GENERIC_ERROR
 */
union AclInfoOrError {
  1: AclInfo aclInfo;
  2: EdenError error;
}

/**
 * Subset of attributes for a single file returned by getAttributesFromFilesV2()
 *
 * When an attribute was not requested the field will be a null optional value.
 * If the attribute was requested, but there was an error computing that
 * specific attribute we will return an Error type for that attribute.
 *
 * See the documentation for the individual attribute types for more
 * (non-exhaustive) information about when certain errors are returned.
 */
struct FileAttributeDataV2 {
  1: optional Sha1OrError sha1;
  2: optional SizeOrError size;
  3: optional SourceControlTypeOrError sourceControlType;
  4: optional ObjectIdOrError objectId;
  5: optional Blake3OrError blake3;
  6: optional DigestSizeOrError digestSize;
  7: optional DigestHashOrError digestHash;
  8: optional MtimeOrError mtime;
  9: optional ModeOrError mode;
  /** Present when UNDER_ACL (bit 10) is requested. */
  10: optional UnderAclOrError underAcl;
  /** Present when ACLs (bit 11) is requested. */
  11: optional AclInfoOrError aclInfo;
}

/**
 * Attributes for a file or information about error encountered when accessing
 * file attributes.
 * If there were errors fetching particular attributes those will be encapsulated
 * in the AttributeOrError type. If there was a general error accessing a file
 * there will be an error here.
 */
union FileAttributeDataOrErrorV2 {
  1: FileAttributeDataV2 fileAttributeData;
  2: EdenError error;
}

/**
 * Mapping from entry name to requested attributes for each of the entries
 * in a certain directory.
 */
union DirListAttributeDataOrError {
  @rust.Type{name = "sorted_vector_map::SortedVectorMap"}
  1: map<PathString, FileAttributeDataOrErrorV2> dirListAttributeData;
  2: EdenError error;
}

/**
 *
 * Ensure that all inflight working copy modification have completed.
 *
 * On some platforms, EdenFS is processing working copy modifications
 * callbacks from the platform in an asynchronous manner, which means that by
 * the time a write/creat/mkdir/unlink/etc syscall returns from the kernel,
 * EdenFS may not have updated its internal state.
 *
 * Thus, an application making changes to the working copy and quickly
 * requesting EdenFS to perform an operation on it will race with EdenFS
 * updating its internal state and may thus get stale data.
 *
 * To avoid this, EdenFS queries need to internally synchronize the working
 * copy before performing the query itself. This structure defines how EdenFS
 * will do this.
 *
 * Applications that care about synchronizing EdenFS up to a certain point in
 * time are expected to set a non-zero syncTimeout once to synchronize EdenFS
 * and then issue all their thrift requests with a syncTimeout of 0.
 */
struct SyncBehavior {
  // How many seconds to wait for the working copy to be synchronized. A value
  // of 0 will not perform any working copy synchronization.
  // If left unset, EdenFS will use a default synchronization timeout.
  1: optional i64 syncTimeoutSeconds;
}

/**
 * Parameters for the getAttributesFromFilesV2() function. By default, results
 * for both files and trees will be returned. Clients can request for only one
 * of trees or files by passing in an AttributesRequestScope.
 */
struct GetAttributesFromFilesParams {
  1: PathString mountPoint;
  2: list<PathString> paths;
  3: RequestedAttributes requestedAttributes;
  4: SyncBehavior sync;
  5: optional AttributesRequestScope scope;
}

/**
 * Return value for the getAttributesFromFilesV2() function.
 * The returned list of attributes corresponds to the input list of
 * paths; eg; res[0] holds the information for paths[0].
 */
struct GetAttributesFromFilesResultV2 {
  1: list<FileAttributeDataOrErrorV2> res;
}

struct ReaddirParams {
  1: PathString mountPoint;
  2: list<PathString> directoryPaths;
  3: RequestedAttributes requestedAttributes;
  4: SyncBehavior sync;
}

/**
 * List of attributes for the entries in the directories specified in
 * directoryPaths. The ordering of the responses corresponds to the ordering
 * the directoryPaths.
 */
struct ReaddirResult {
  1: list<DirListAttributeDataOrError> dirLists;
}

/** reference a point in time in the journal.
 * This can be used to reason about a point in time in a given mount point.
 * The mountGeneration value is opaque to the client.
 */
struct JournalPosition {
  /** An opaque but unique number within the scope of a given mount point.
   * This is used to determine when sequenceNumber has been invalidated. */
  1: i64 mountGeneration;

  /** Monotonically incrementing number
   * Each journalled change causes this number to increment. */
  2: unsigned64 sequenceNumber;

  /** Records the snapshot hash at the appropriate point in the journal */
  3: ThriftRootId snapshotHash;
}

/**
 * Holds information about a set of paths that changed between two points.
 * fromPosition, toPosition define the time window.
 * paths holds the list of paths that changed in that window.
 *
 * This type is quasi-deprecated. It has multiple API problems and should be
 * rethought when we have a chance to make a breaking change.
 */
struct FileDelta {
  /**
   * The fromPosition passed to getFilesChangedSince
   */
  1: JournalPosition fromPosition;
  /**
   * The current position at the time that getFilesChangedSince was called
  */
  2: JournalPosition toPosition;
  /**
   * The union of changedPaths and createdPaths contains the total set of paths
   * changed in the overlay between fromPosition and toPosition.
   * Disjoint with createdPaths.
   */
  3: list<PathString> changedPaths;
  /**
   * The set of paths created between fromPosition and toPosition.
   * Used by Watchman to search for cookies and to populate its 'new' field.
   * Disjoint with changedPaths.
   */
  4: list<PathString> createdPaths;
  /**
   * Deprecated - always empty.
   */
  5: list<PathString> removedPaths;
  /**
   * When fromPosition.snapshotHash != toPosition.snapshotHash this holds
   * the union of the set of files whose ScmFileStatus differed from the
   * committed fromPosition hash before the hash changed, and the set of
   * files whose ScmFileStatus differed from the committed toPosition hash
   * after the hash was changed.  This list of files represents files
   * whose state may have changed as part of an update operation, but
   * in ways that may not be able to be extracted solely by performing
   * source control diff operations on the from/to hashes.
   */
  6: list<PathString> uncleanPaths;
  /**
   * Contains the list of commit transitions in this range. If only files
   * have been changed, the list has one entry. Otherwise, it has size N + 1,
   * where N is the number of checkout operations.
   *
   * This list's items may not be unique: [A, B, A] is a common sequence,
   * and [A, B, C] has a different meaning than [A, C, B].
   *
   * Subsumes fromPosition.snapshotHash and toPosition.snapshotHash.
   */
  7: list<ThriftRootId> snapshotTransitions;
}

struct DebugGetRawJournalParams {
  1: PathString mountPoint;
  2: optional i32 limit;
  3: i32 fromSequenceNumber;
}

struct DebugPathChangeInfo {
  1: bool existedBefore;
  2: bool existedAfter;
}

/**
 * A fairly direct modeling of the underlying JournalDelta data structure.
 */
struct DebugJournalDelta {
  1: JournalPosition fromPosition;
  2: JournalPosition toPosition;
  3: map<PathString, DebugPathChangeInfo> changedPaths;
  4: set<PathString> uncleanPaths;
}

struct DebugGetRawJournalResponse {
  2: list<DebugJournalDelta> allDeltas;
}

/**
 * Classifies the change of the state of a file between and old and new state
 * of the repository. Most commonly, the "old state" is the parent commit while
 * the "new state" is the working copy.
 */
enum ScmFileStatus {
  /**
   * File is present in the new state, but was not present in old state.
   */
  ADDED = 0x0,

  /**
   * File is present in both the new and old states, but its contents or
   * file permissions have changed.
   */
  MODIFIED = 0x1,

  /**
   * File was present in the old state, but is not present in the new state.
   */
  REMOVED = 0x2,

  /**
   * File is present in the new state, but it is ignored according to the rules
   * of the new state.
   */
  IGNORED = 0x3,
}

struct ScmStatus {
  1: map<PathString, ScmFileStatus> entries;

  /**
   * A map of { path -> error message }
   *
   * If any errors occurred while computing the diff they will be reported here.
   * The results listed in the entries field may not be accurate for any paths
   * listed in this error field.
   *
   * This map will be empty if no errors occurred.
   */
  2: map<PathString, string> errors;
}

/** Option for use with checkOutRevision(). */
enum CheckoutMode {
  /**
   * Perform a "normal" checkout, analogous to `hg checkout` in Mercurial. Files
   * in the working copy will be changed to reflect the destination snapshot,
   * though files with conflicts will not be modified.
   */
  NORMAL = 0,

  /**
   * Do not checkout: exercise the checkout logic to discover potential
   * conflicts.
   */
  DRY_RUN = 1,

  /**
   * Perform a "forced" checkout, analogous to `hg checkout --clean` in
   * Mercurial. Conflicts between the working copy and destination snapshot will
   * be forcibly ignored in favor of the state of the new snapshot.
   */
  FORCE = 2,
}

enum ConflictType {
  /**
   * We failed to update this particular path due to an error.
   */
  ERROR = 0,
  /**
   * A locally modified file was deleted in the new Tree.
   */
  MODIFIED_REMOVED = 1,
  /**
   * An untracked local file exists in the new Tree.
   * The new entry may be a file or directory.
   */
  UNTRACKED_ADDED = 2,
  /**
   * The file was removed locally, but modified in the new Tree.
   */
  REMOVED_MODIFIED = 3,
  /**
   * The file was removed locally, and also removed in the new Tree.
   */
  MISSING_REMOVED = 4,
  /**
   * A locally modified file was modified in the new Tree.
   * This may be contents modifications, or a file type change (directory to
   * file or vice-versa), or permissions changes.
   */
  MODIFIED_MODIFIED = 5,
  /**
   * A directory was supposed to be removed or replaced with a file,
   * but it contains untracked files preventing us from updating it.
   */
  DIRECTORY_NOT_EMPTY = 6,
  /**
   * A path would become restricted by the destination checkout while local
   * state at that path remains visible and must be reported as a conflict.
   */
  VISIBLE_RESTRICTED = 7,
}

/**
 * Details about conflicts or errors that occurred during a checkout operation
 */
struct CheckoutConflict {
  1: PathString path;
  2: ConflictType type;
  3: string message;
  4: Dtype dtype;
}

struct ScmBlobMetadata {
  1: i64 size;
  2: BinaryHash contentsSha1;
}

struct ScmTreeEntry {
  1: binary name;
  2: i32 mode;
  3: ThriftObjectId id;
}

/*
 * Bits passed into debugInodeStatus to control the result set.
 * 0 is legacy behavior, equivalent to DIS_REQUIRE_LOADED | DIS_COMPUTE_BLOB_SIZES
 */

/**
 * No effect other than avoiding the legacy behavior.
 */
const i64 DIS_ENABLE_FLAGS = 1;

/**
 * Only return inodes currently loaded in memory.
 */
const i64 DIS_REQUIRE_LOADED = 2;

/**
 * Only return materialized inodes.
 */
const i64 DIS_REQUIRE_MATERIALIZED = 4;

/**
 * Return accurate blob sizes, which may require fetching blob metadata from
 * the backing store.
 */
const i64 DIS_COMPUTE_BLOB_SIZES = 8;

/**
 * Returns accurate mode_t bits, including ownership. When unset, only
 * the dtype bits are set.
 */
const i64 DIS_COMPUTE_ACCURATE_MODE = 16;

/**
 * Only return data for the passed in directory, do not attempt to recurse down
 * to its childrens.
 */
const i64 DIS_NOT_RECURSIVE = 32;

struct TreeInodeEntryDebugInfo {
  /**
   * The entry name.  This is just a PathComponent, not the full path
   */
  1: binary name;
  /**
   * The inode number, or 0 if no inode number has been assigned to
   * this entry
   */
  2: i64 inodeNumber;
  /**
   * The entry mode_t value
   */
  3: i32 mode;
  /**
   * True if an InodeBase object exists for this inode or not.
   */
  4: bool loaded;
  /**
   * True if an the inode is materialized in the overlay
   */
  5: bool materialized;
  /**
   * If materialized is false, hash contains the ID of the underlying source
   * control Blob or Tree.
   */
  6: ThriftObjectId hash;
  /**
   * Size of the file in bytes. It won't be set for directories.
   */
  7: optional i64 fileSize;
}

struct GetFetchedFilesResult {
  1: map<string, set<PathString>> fetchedFilePaths;
}

struct WorkingDirectoryParents {
  1: ThriftRootId parent1;
  // This field is never used by EdenFS.
  2: optional ThriftRootId parent2;
}

struct TreeInodeDebugInfo {
  1: i64 inodeNumber;
  2: binary path;
  3: bool materialized;
  // Traditionally, treeHash was a 20-byte binary hash, but now it's an
  // arbitrary-length human-readable string.
  4: ThriftObjectId treeHash;
  5: list<TreeInodeEntryDebugInfo> entries;
  6: i64 refcount;
}

struct InodePathDebugInfo {
  1: PathString path;
  2: bool loaded;
  3: bool linked;
}

/**
 * Where debug methods should fetch data from.
 */
enum DataFetchOrigin {
  NOWHERE = 0,
  ANYWHERE = 1,
  MEMORY_CACHE = 2,
  // EdenFS has no on-disk object store of its own; nothing is served from
  // this origin. The value is reserved so the later ones stay stable.
  DISK_CACHE = 4,
  LOCAL_BACKING_STORE = 8,
  REMOTE_BACKING_STORE = 16,
/* NEXT_WHERE = 2^x */
}

struct DebugGetScmBlobRequest {
  1: MountId mountId;
  # id of the blob we would like to fetch
  2: ThriftObjectId id;
  # where we should fetch the blob from
  3: DataFetchOriginSet origins; # DataFetchOrigin
}

union ScmBlobOrError {
  1: binary blob;
  2: EdenError error;
}

struct ScmBlobWithOrigin {
  # the blob data
  1: ScmBlobOrError blob;
  # where the blob was fetched from
  2: DataFetchOrigin origin;
}

struct DebugGetScmBlobResponse {
  1: list<ScmBlobWithOrigin> blobs;
}

struct DebugGetBlobMetadataRequest {
  1: MountId mountId;
  # id of the blob we would like to fetch metadata for
  2: ThriftObjectId id;
  # where we should fetch the blob metadata from
  3: DataFetchOriginSet origins; # DataFetchOrigin
}

union BlobMetadataOrError {
  1: ScmBlobMetadata metadata;
  2: EdenError error;
}

struct BlobMetadataWithOrigin {
  # the blob data
  1: BlobMetadataOrError metadata;
  # where the blob was fetched from
  2: DataFetchOrigin origin;
}

struct DebugGetBlobMetadataResponse {
  1: list<BlobMetadataWithOrigin> metadatas;
}

struct DebugGetScmTreeRequest {
  1: MountId mountId;
  # id of the blob we would like to fetch SCM tree for
  2: ThriftObjectId id;
  # where we should fetch the blob SCM tree from
  3: DataFetchOriginSet origins; # DataFetchOrigin
}

union ScmTreeOrError {
  1: list<ScmTreeEntry> treeEntries;
  2: EdenError error;
}

struct TreeAux {
  1: DigestSizeOrError digestSize;
  2: DigestHashOrError digestHash;
}

struct ScmTreeWithOrigin {
  # the SCM tree data
  1: ScmTreeOrError scmTreeData;
  # where the SCM tree was fetched from
  2: DataFetchOrigin origin;
  # include the Tree Aux when it's available
  3: optional TreeAux treeAux;
}

struct DebugGetScmTreeResponse {
  1: list<ScmTreeWithOrigin> trees;
}

struct ActivityRecorderResult {
  // 0 if the operation has failed. For example,
  // fail to start recording due to file permission issue
  // or fail to stop recording due to no active subscriber.
  1: i64 unique;
  2: optional PathString path;
}

struct ListActivityRecordingsResult {
  1: list<ActivityRecorderResult> recordings;
}

struct SetLogLevelResult {
  1: bool categoryCreated;
}

struct JournalInfo {
  1: i64 entryCount;
  // The estimated memory used by the journal in bytes
  2: i64 memoryUsage;
  // The duration of the journal in seconds
  3: i64 durationSeconds;
}

/**
 * Struct to store Information about inodes in a mount point.
 */
struct MountInodeInfo {
  2: i64 unloadedInodeCount;
  4: i64 loadedFileCount;
  5: i64 loadedTreeCount;
}

struct CacheStats {
  1: i64 entryCount;
  2: i64 totalSizeInBytes;
  3: i64 hitCount;
  4: i64 missCount;
  5: i64 evictionCount;
  6: i64 dropCount;
}

/*
 * Bits that control the stats returned from  getStatInfo
 */
const i64 STATS_MOUNTS_STATS = 0x1;
const i64 STATS_COUNTERS = 0x2;
const i64 STATS_SMAPS = 0x4;
const i64 STATS_PRIVATE_BYTES = 0x8;
const i64 STATS_RSS_BYTES = 0x10;
const i64 STATS_CACHE_STATS = 0x20;
const i64 STATS_ALL = 0xFFFF;

/**
 * Struct to store fb303 counters from ServiceData.getCounters() and inode
 * information of all the mount points.
 */
struct InternalStats {
  /**
  * fbf303 counter of inodes unloaded by periodic job.
  * Populated if STATS_COUNTERS is set.
  */
  1: optional i64 periodicUnloadCount;
  /**
   * counters is the list of fb303 counters, key is the counter name, value is the
   * counter value.
   * Populated if STATS_COUNTERS is set.
   */
  2: optional map<string, i64> counters;
  /**
   * mountPointInfo is a map whose key is the path of the mount point and value
   * is the details like number of loaded inodes,unloaded inodes in that mount
   * and number of materialized inodes in that mountpoint.
   * Populated if STATS_MOUNTS_STATS is set.
   */
  3: optional map<PathString, MountInodeInfo> mountPointInfo;
  /**
   * Linux-only: the contents of /proc/self/smaps, to be parsed by the caller.
   * Populated if STATS_SMAPS is set.
   */
  4: optional binary smaps;
  /**
   * Linux-only: privateBytes populated from contents of /proc/self/smaps.
   * Populated with current value (the fb303 counters value is an average).
   * Populated if STATS_PRIVATE_BYTES is set.
   */
  5: optional i64 privateBytes;
  /**
   * Linux-only: vmRSS bytes is populated from contents of /proc/self/stats.
   * Populated with current value (the fb303 counters value is an average).
   * Populated if STATS_RSS_BYTES is set.
   */
  6: optional i64 vmRSSBytes;
  /**
   * Statistics about the in-memory blob cache.
   * Populated if STATS_CACHE_STATS is set.
   */
  7: optional CacheStats blobCacheStats;
  /**
   * mountPointJournalInfo is a map whose key is the path of the mount point
   * and whose value is information about the journal on that mount
   * Populated if STATS_MOUNTS_STATS is set.
   */
  8: optional map<PathString, JournalInfo> mountPointJournalInfo;
  /**
   * Statistics about the in-memory tree cache.
   * Populated if STATS_CACHE_STATS is set.
   */
  9: optional CacheStats treeCacheStats;
}

/**
 * Common timestamps for every trace event, used to measure durations and
 * display wall clock time.
 */
struct TraceEventTimes {
  // Nanoseconds since epoch.
  1: i64 timestamp;
  // Nanoseconds since arbitrary clock base, used for computing request
  // durations between start and finish.
  2: i64 monotonic_time_ns;
}

enum ThriftRequestEventType {
  UNKNOWN = 0,
  START = 1,
  FINISH = 2,
}

struct ThriftRequestEvent {
  1: TraceEventTimes times;
  2: ThriftRequestEventType eventType;
  3: ThriftRequestMetadata requestMetadata;
}

/**
 * Return value for the getRetroactiveThriftRequestEvents() function.
 */
struct GetRetroactiveThriftRequestEventsResult {
  1: list<ThriftRequestEvent> events;
}

struct RequestInfo {
  // The pid that originated this request.
  1: optional pid_t pid;
  // If available, the binary name corresponding to `pid`.
  2: optional string processName;
}

struct ClientRequestInfo {
  1: string correlator;
  2: string entry_point;
}

enum HgEventType {
  UNKNOWN = 0,
  QUEUE = 1,
  START = 2,
  FINISH = 3,
}

enum HgResourceType {
  UNKNOWN = 0,
  BLOB = 1,
  TREE = 2,
  BLOBMETA = 3,
  TREEMETA = 4,
  BLOBBATCH = 5,
}

enum HgImportPriority {
  LOW = 0,
  NORMAL = 1,
  HIGH = 2,
}

enum HgImportCause {
  UNKNOWN = 0,
  FS = 1,
  THRIFT = 2,
  PREFETCH = 3,
}

enum FetchedSource {
  LOCAL = 0,
  REMOTE = 1,
  // The data is fetched. However, the fetch mode was AllowRemote
  // and on the Eden side the source of the fetch is unknown.
  // It could be local or remote
  UNKNOWN = 2,
  // The data is not fetched yet.
  // We don't know the source on some of the Sapling events. For example,
  // on the start events: before we fetch the data we don't know where
  // we will be able to find it
  NOT_AVAILABLE_YET = 3,
}

struct HgEvent {
  1: TraceEventTimes times;

  2: HgEventType eventType;
  3: HgResourceType resourceType;

  4: i64 unique;

  // HG manifest node ID as 40-character hex string.
  5: string manifestNodeId;
  6: binary path;

  7: optional RequestInfo requestInfo;
  8: HgImportPriority importPriority;
  9: HgImportCause importCause;
  10: FetchedSource fetchedSource;
}

/**
 * Parameters for the getRetroactiveHgEvents() function.
 */
struct GetRetroactiveHgEventsParams {
  1: PathString mountPoint;
}

/**
 * Return value for the getRetroactiveHgEvents() function.
 */
struct GetRetroactiveHgEventsResult {
  1: list<HgEvent> events;
}

enum InodeType {
  TREE = 0,
  FILE = 1,
}

enum InodeEventType {
  UNKNOWN = 0,
  MATERIALIZE = 1,
  LOAD = 2,
}

enum InodeEventProgress {
  START = 0,
  END = 1,
  FAIL = 2,
}

struct InodeEvent {
  2: i64 ino;
  3: InodeType inodeType;
  4: InodeEventType eventType;
  // Duration is in microseconds (μs)
  5: i64 duration;
  6: InodeEventProgress progress;
  7: TraceEventTimes times;
  8: PathString path;
}

struct TaskEvent {
  1: TraceEventTimes times;
  2: string name;
  3: string threadName;
  4: i64 threadId;
  // Both duration and start are in microseconds (μs)
  5: unsigned64 duration;
  6: unsigned64 start;
}

/**
 * Parameters for the getRetroactiveInodeEvents() function.
 */
struct GetRetroactiveInodeEventsParams {
  1: PathString mountPoint;
}

/**
 * Return value for the getRetroactiveInodeEvents() function.
 */
struct GetRetroactiveInodeEventsResult {
  1: list<InodeEvent> events;
}

struct FuseCall {
  // This field is deprecated because its use is not worth the TraceBus
  // storage. It may be brought back in some other form.
  //1: i32 len;

  2: i32 opcode;
  // FUSE supplies a unique ID, but it is recycled so quickly that it's not
  // very useful. We report our own process-unique ID.
  3: i64 unique;
  4: i64 nodeid;
  5: i32 uid;
  6: i32 gid;
  7: pid_t pid;

  8: string opcodeName;
  9: optional string processName;
}

struct NfsCall {
  1: i32 xid;
  2: i32 procNumber;
  3: string procName;
}

enum PrjfsTraceCallType {
  INVALID = 0,
  /* Write operations */
  FILE_OPENED = 1,
  NEW_FILE_CREATED = 2,
  FILE_OVERWRITTEN = 3,
  PRE_DELETE = 4,
  PRE_RENAME = 5,
  PRE_SET_HARDLINK = 6,
  FILE_RENAMED = 7,
  HARDLINK_CREATED = 8,
  FILE_HANDLE_CLOSED_NO_MODIFICATION = 9,
  FILE_HANDLE_CLOSED_FILE_MODIFIED = 10,
  FILE_HANDLE_CLOSED_FILE_DELETED = 11,
  FILE_PRE_CONVERT_TO_FULL = 12,
  /* Read operations */
  START_ENUMERATION = 13,
  END_ENUMERATION = 14,
  GET_ENUMERATION_DATA = 15,
  GET_PLACEHOLDER_INFO = 16,
  QUERY_FILE_NAME = 17,
  GET_FILE_DATA = 18,
  CANCEL_COMMAND = 19,
}

struct PrjfsCall {
  1: i32 commandId;
  2: i32 pid;
  3: PrjfsTraceCallType callType;
}

/**
 * Metadata about an in-progress Thrift request.
 */
struct ThriftRequestMetadata {
  1: i64 requestId;
  2: string method;
  3: pid_t clientPid;
}

struct GetConfigParams {
  // Whether to reload the config from disk to make sure it is up-to-date
  1: eden_config.ConfigReloadBehavior reload = eden_config.ConfigReloadBehavior.AutoReload;
}

struct GetStatInfoParams {
  // a bitset that indicates the requested stats. 0 indicates all stats are requested.
  1: i64 statsMask;
}

struct DebugInvalidateRequest {
  1: MountId mount;
  // Relative path in the repo to recursively invalidate
  2: PathString path;
  // Files last accessed before now-age will be invalidated. A zero age means
  // all inodes.
  3: TimeSpec age;
  4: SyncBehavior sync;
  // Run the invalidation in the background.
  5: bool background;
}

struct DebugInvalidateResponse {
  // Number of files that were successfully invalidated. When a background
  // invalidation is requested, this will always be 0.
  1: unsigned64 numInvalidated;
}

/**
 * A representation of the system-dependent dirent::d_type field.
 * The bits and their interpretation is system dependent.
 * This value is u8 on all systems that implement it.  We
 * use i16 to pass this through thrift, which doesn't have unsigned
 * numbers
 */
typedef i16 OsDtype

enum SourceControlType {
  TREE = 0,
  REGULAR_FILE = 1,
  EXECUTABLE_FILE = 2,
  SYMLINK = 3,
  UNKNOWN = 4, // File types that can not be versioned in source control.
// Currently includes things like FIFOs and sockets.
}

/**
 * These numbers match up with Linux and macOS.
 * Windows doesn't have dtype_t, but a subset of these map to and from
 * the GetFileType and dwFileAttributes equivalents.
 *
 * Dtype and OsDtype can be cast between each other on all platforms.
 */
enum Dtype {
  UNKNOWN = 0,
  FIFO = 1, // DT_FIFO
  CHAR = 2, // DT_CHR
  DIR = 4, // DT_DIR
  BLOCK = 6, // DT_BLK
  REGULAR = 8, // DT_REG
  LINK = 10, // DT_LNK
  SOCKET = 12, // DT_SOCK
  WHITEOUT = 14, // DT_WHT
}

/**
 * Method used to warm the OS page cache after prefetch.
 */
enum PreloadMethod {
  // posix_fadvise(WILLNEED) or fcntl(F_RDADVISE) on macOS
  FADVISE = 0,
  // readahead() (Linux only)
  READAHEAD = 1,
  // pread() loop; reliable on FUSE filesystems
  READ = 2,
  // mmap() + madvise(MADV_WILLNEED)
  MMAP = 3,
}

/**
 * DEPRECATED: predictive prefetching has been removed. This struct is kept
 * only for wire compatibility and is ignored by the daemon.
 */
struct PredictiveFetch {
  // Number of directories to glob.
  1: optional i32 numTopDirectories;
  // Fetch the most accessed directories by user specified. If not specified, user is derived from the server state.
  2: optional string user;
  // Fetch the most accessed directories in repository specified. If not specified, repo is derived from the mount.
  3: optional string repo;
  // Optional query parameter: fetch top most accessed directories with specified Operating System
  4: optional string os;
  // Optional query parameter: fetch top most accessed directories after startTime
  5: optional unsigned64 startTime;
  // Optional query parameter: fetch top most accessed directories before endTime
  6: optional unsigned64 endTime;
}

/** Params for prefetchFiles(). */
struct PrefetchParams {
  1: PathString mountPoint;
  2: list<string> globs;
  // If set, don't prefetch matching blobs. Only prefetch trees.
  3: bool directoriesOnly = false;
  // Commit hashes for the revisions against which the globs should be
  // evaluated, if this is empty then globFiles will fall back to using only
  // the current revision.
  4: list<ThriftRootId> revisions;
  // The directory from which the glob should be evaluated. Defaults to the
  // repository root.
  5: PathString searchRoot;
  // If set, will run the prefetch but will not wait for the result.
  6: bool background = false;
  // DEPRECATED: ignored.
  7: optional PredictiveFetch predictiveGlob;
  // When true, returns list of prefetched files.
  8: bool returnPrefetchedFiles = false;
  // When true, return PrefetchStats in PrefetchResult.
  9: bool returnStats = false;
  // When true, after prefetching blobs to hgcache, also warm the OS page cache
  // by reading files through the mount. Without preloadProgress this is
  // fire-and-forget: the RPC returns when prefetch completes and the preload
  // phase continues in the background with no way to observe it.
  10: bool preload = false;
  // When true (and preload is true), return an operationId for progress
  // queries via queryPreloadProgress().
  11: bool preloadProgress = false;
  // Method to use for page cache preloading (only used when preload is true).
  // Defaults to READ (pread loop) which reliably populates the page cache
  // on FUSE mounts. Advisory methods (fadvise, readahead) don't persist
  // under memory pressure on FUSE.
  12: PreloadMethod preloadMethod = PreloadMethod.READ;
}

struct PrefetchStats {
  1: i64 treesFromMemoryCache;
  2: i64 treesFromDiskCache;
  3: i64 treesFromNetwork;
  4: i64 blobsFromMemoryCache;
  5: i64 blobsFromDiskCache;
  6: i64 blobsFromNetwork;

  7: i64 treeBytesFromMemoryCache;
  8: i64 treeBytesFromDiskCache;
  9: i64 treeBytesFromNetwork;
  10: i64 blobBytesFromMemoryCache;
  11: i64 blobBytesFromDiskCache;
  12: i64 blobBytesFromNetwork;

  13: i64 totalDurationMs;

  14: double cacheHitRate;

  16: i64 filesPrefetched;
  17: i64 filesFailed;
}

/**
 * Disk-usage state of one cache (blob/tree/lfs) within HgCacheStats.
 */
enum CacheUsageState {
  // No cache is configured for this store (e.g. no remotefilelog.cachepath).
  NOT_CONFIGURED = 0,
  // This store implementation does not support usage reporting.
  UNSUPPORTED = 1,
  // A cache is configured; see the paired *BytesUsed/*BytesLimit fields.
  AVAILABLE = 2,
  // A cache is configured, but its usage could not be measured due to an
  // error (e.g. corrupted on-disk data). The failure reason is logged
  // server-side, not carried on this value.
  UNAVAILABLE = 3,
}

/**
 * Per-repo hgcache usage and limits (blob, tree, and LFS caches).
 *
 * Each of blob/tree/lfs independently reports NOT_CONFIGURED, UNSUPPORTED,
 * AVAILABLE, or UNAVAILABLE via its *State field; the paired
 * *BytesUsed/*BytesLimit fields are only meaningful when *State is
 * AVAILABLE (0 otherwise). An AVAILABLE *BytesLimit of -1 means uncapped.
 *
 * lfsBytes* covers only the indexedlog-backed portion of the LFS cache
 * (lfs/blobs, lfs/pointers); the loose-file lfs/objects directory is
 * excluded, since sizing it needs a full directory walk.
 */
struct HgCacheStats {
  1: PathString cachePath;
  // Whether cachePath is meaningful; false when no cache is configured at
  // all (cachePath is empty in that case, not merely unset).
  2: bool cachePathConfigured;
  3: CacheUsageState blobState;
  4: i64 blobBytesUsed;
  5: i64 blobBytesLimit;
  6: CacheUsageState treeState;
  7: i64 treeBytesUsed;
  8: i64 treeBytesLimit;
  9: CacheUsageState lfsState;
  10: i64 lfsBytesUsed;
  11: i64 lfsBytesLimit;
}

/** Result for prefetchFiles(). */
struct PrefetchResult {
  1: optional Glob prefetchedFiles;
  2: optional PrefetchStats stats;
  // ID for a long-running preload operation.
  // Only populated when preload=true and preloadProgress=true.
  3: optional string operationId;
  // Set only when returnStats=true and the hgcache stats lookup succeeds;
  // absent (not merely zeroed) when returnStats=false or the lookup fails.
  4: optional HgCacheStats cacheStats;
}

/**
 * Progress for a page cache preload operation.
 * Returned by queryPreloadProgress().
 */
struct PreloadProgress {
  // Preload phase progress.
  1: i64 processed;
  2: i64 total;
  3: bool isDone;
  4: optional string error;
  // Duration of the preload operation in milliseconds (set when isDone=true).
  5: optional i64 durationMs;

  // Prefetch phase progress (fetching blobs from network).
  6: i64 prefetchProcessed;
  7: i64 prefetchTotal;
  8: bool prefetchDone;
  9: i64 prefetchBytesProcessed;
  10: i64 prefetchBytesTotal;
}

/** Params for queryPreloadProgress(). */
struct PreloadProgressRequest {
  // The operationId returned by prefetchFilesV2 when preload=true and
  // preloadProgress=true.
  1: string operationId;
}

/** Result for queryPreloadProgress(). */
struct PreloadProgressResponse {
  // False when the operationId is not known to the daemon: it was never
  // issued, its operation completed and was already swept (completed entries
  // are only retained briefly for late polls), or the daemon restarted (the
  // registry is in-memory). Clients must treat found=false as "no longer
  // observable", NOT as successful completion.
  1: bool found;
  // Progress for the operation. Only set when found=true.
  2: optional PreloadProgress progress;
}

/** Params for globFiles(). */
struct GlobParams {
  1: PathString mountPoint;
  2: list<string> globs;
  3: bool includeDotfiles;
  // if true, prefetch matching blobs
  4: bool prefetchFiles;
  // if true, don't populate matchingFiles in the Glob
  // results.  This only really makes sense with prefetchFiles.
  5: bool suppressFileList;
  6: bool wantDtype;
  // Commit hashes for the revisions against which the globs should be
  // evaluated, if this is empty then globFiles will fall back to using only
  // the current revision.
  // If eden moves away from commit hashes this may become the tree hash
  // for the root tree against which this glob should be evaluated.
  // There should be no duplicates in this list. If there are then
  // there maybe duplicate matchingFile and originHash pairs in the corresponding
  // output Glob.
  7: list<ThriftRootId> revisions;
  // This has no effect.
  8: bool prefetchMetadata = true;
  // The directory from which the glob should be evaluated. Defaults to the
  // repository root.
  9: PathString searchRoot;
  // If set, will run the prefetch but will not wait for the result.
  10: bool background = false;
  // DEPRECATED: ignored.
  11: optional PredictiveFetch predictiveGlob;
  // Normally the returned file list will contain both files and directories.
  // Some clients would like to see only lists of files, this option tells us
  // whether to filter or not.
  // Note for Eden developers: when this is false the matchingFiles fileList
  // will not actually be the list that is fed into the backing store to prefetch.
  12: bool listOnlyFiles = false;
  // Should this glob query also synchronize the working copy?
  13: SyncBehavior sync;
}

struct Glob {
  /**
   * matchingFiles can contain duplicate values and is not guaranteed to be
   * sorted. However, no duplicates may have the same originCommits (note this
   * is not true should the input GlobParams contain duplicate revisions) .
   */
  1: list<GlobPathValue> matchingFiles;
  2: list<OsDtype> dtypes;
  /**
   * Currently these are the commit hash for the commit to which this file
   * belongs. But should eden move away from commit hashes this may become
   * the tree hash of the root tree to which this file belongs.
   */
  3: list<binary> originHashes;
}

struct AccessCounts {
  1: i64 fsChannelTotal;
  2: i64 fsChannelReads;
  3: i64 fsChannelWrites;
  4: i64 fsChannelBackingStoreImports;
  5: i64 fsChannelDurationNs;
  6: i64 fsChannelMemoryCacheImports;
  7: i64 fsChannelDiskCacheImports;
}

struct MountAccesses {
  1: map<pid_t, AccessCounts> accessCountsByPid;
  2: map<pid_t, i64> fetchCountsByPid;
}

struct GetAccessCountsResult {
  1: map<pid_t, binary> cmdsByPid;
  2: map<PathString, MountAccesses> accessesByMount;
  // TODO: Count the number of thrift requests
  // 3: map<pid_t, AccessCount> thriftAccesses
}

enum TracePointEvent {
  // Start of a new block
  START = 0,
  // End of a block
  STOP = 1,
}

struct TracePoint {
  // Holds nanoseconds since the epoch
  1: i64 timestamp;
  // Opaque identifier for the entire trace - used to associate this
  // tracepoint with other tracepoints across an entire request
  2: i64 traceId;
  // Opaque identifier for this "block" where a block is some logical
  // piece of work with a well-defined start and stop point
  3: i64 blockId;
  // Opaque identifier for the parent block from which the current
  // block was constructed - used to create causal relationships
  // between blocks
  4: i64 parentBlockId;
  // The name of the block, only set on the tracepoint starting the
  // block, must point to a statically allocated cstring
  5: string name = "";
  // What event this trace point represents
  6: TracePointEvent event;
  // Thread ID of where this block started
  7: i64 threadId;
}

struct FaultDefinition {
  1: string keyClass;
  2: string keyValueRegex;
  // If count is non-zero this fault will automatically expire after it has
  // been hit count times.
  3: i64 count;
  // injectFault applies the first behavior that is set, in this order:
  // block, blockWithCancel, kill, unblock, then the delay and error fields.
  // Fields for later behaviors are ignored.
  //
  // If block is true the fault will block until explicitly unblocked later.
  4: bool block;
  5: i64 delayMilliseconds;
  6: optional string errorType;
  7: optional string errorMessage;
  // If kill is true the fault will exit the process ungracefully.
  8: bool kill;
  // If blockWithCancel is true the fault will block until the request is cancelled or times out.
  9: bool blockWithCancel;
  // If both unblock fields are set, hitting this fault releases matching
  // blocked checks instead of applying the delay and error fields. Setting
  // only one of them is rejected with EINVAL.
  10: optional string unblockKeyClass;
  11: optional string unblockKeyValueRegex;
}

struct RemoveFaultArg {
  1: string keyClass;
  2: string keyValueRegex;
}

struct UnblockFaultArg {
  1: optional string keyClass;
  2: optional string keyValueRegex;
  3: optional string errorType;
  4: optional string errorMessage;
}

struct GetScmStatusResult {
  1: ScmStatus status;
  // The version of the EdenFS daemon.
  // This is returned since we usually want status calls to be able to check
  // the current EdenFS version and warn the user if EdenFS is running an old
  // or known-bad version.
  2: string version;
}

/**
 * Sometimes additional modifiers need to be applied to the RootID that Eden
 * receives from clients. This structure contains any such option and should
 * only be extended with optional fields.
 */
struct RootIdOptions {
  /**
   * The ID of the filter that should be applied to the supplied RootId. The
   * filter determines which entries in the repository should be hidden from
   * the working copy.
   */
  2: optional binary fid;
}

struct GetScmStatusParams {
  /**
   * The Eden checkout to query
   */
  1: PathString mountPoint;

  /**
   * The commit ID of the current working directory parent commit.
   *
   * An error will be returned if this is not actually the current parent
   * commit.  This behavior exists to support callers that do not perform their
   * own external synchronization around access to the current parent commit,
   * like Mercurial.
   */
  2: ThriftRootId commit;

  /**
   * Whether ignored files should be reported in the results.
   *
   * Some special source-control related files (e.g., inside the .hg or .git
   * directory) will never be reported even when listIgnored is true.
   */
  3: bool listIgnored = false;

  // Pass unique identifier of this request's caller.
  4: optional ClientRequestInfo cri;

  5: optional RootIdOptions rootIdOptions;
}

/**
  * BackingStore object type. Caller will response to verify the type of the content
  * matching the parameters passed. Exception will be thrown if type mismatch.
  */
enum ObjectType {
  TREE = 0,
  REGULAR_FILE = 1,
  EXECUTABLE_FILE = 2,
  SYMLINK = 3,
}

struct SetPathObjectIdObject {
  1: PathString path;
  2: ThriftObjectId objectId;
  3: ObjectType type;
}

// Any new use case should try to avoid using path, objectId and type, they will be deprecated.
// Please use objects instead which would batch requests. If both path/objectId/type and objects
// both present, the singular will be added to the objects.
struct SetPathObjectIdParams {
  1: PathString mountPoint;
  // TODO: deprecate path, objectId and type
  2: PathString path;
  3: ThriftObjectId objectId;
  4: ObjectType type;
  5: CheckoutMode mode;
  // Extra request information. i.e. build uuid, cache session id.
  6: optional map<string, string> requestInfo;
  7: list<SetPathObjectIdObject> objects;
}

struct SetPathObjectIdResult {
  1: list<CheckoutConflict> conflicts;
}

struct CheckOutRevisionParams {
  /**
   * The hg root manifest that corresponds to the commit (if known).
   *
   * When a commit is newly created, EdenFS won't know the commit
   * to root-manifest mapping for the commit, and won't be able to find
   * out from the import helper until the import helper re-opens the
   * repo.  To speed this up, Mercurial clients may optionally provide
   * the hash of the root manifest directly, so that EdenFS doesn't
   * need to look it up.
   */
  1: optional BinaryHash hgRootManifest;

  // Pass unique identifier of this request's caller.
  2: optional ClientRequestInfo cri;

  3: optional RootIdOptions rootIdOptions;
}

struct ResetParentCommitsParams {
  /**
   * The hg root manifest that corresponds to the commit (if known).
   *
   * When a commit is newly created, EdenFS won't know the commit
   * to root-manifest mapping for the commit, and won't be able to find
   * out from the import helper until the import helper re-opens the
   * repo.  To speed this up, Mercurial clients may optionally provide
   * the hash of the root manifest directly, so that EdenFS doesn't
   * need to look it up.
   */
  1: optional BinaryHash hgRootManifest;

  // Pass unique identifier of this request's caller.
  2: optional ClientRequestInfo cri;

  3: optional RootIdOptions rootIdOptions;
}

struct GetCurrentSnapshotInfoRequest {
  // Mount for which you want information.
  1: MountId mountId;
  // Pass unique identifier of this request's caller.
  2: optional ClientRequestInfo cri;
}

struct GetCurrentSnapshotInfoResponse {
  2: optional binary fid;
}

struct RemoveRecursivelyParams {
  1: PathString mountPoint;
  2: PathString path;
  3: SyncBehavior sync;
}

struct SynchronizeWorkingCopyParams {
  1: SyncBehavior sync;
}

struct EnsureMaterializedParams {
  1: PathString mountPoint;
  2: list<PathString> paths;
  // Materialize on the background and do not block.
  3: bool background = false;
  // Also materialize symlink target if the target is also in the same mount
  4: bool followSymlink = false;
  5: SyncBehavior sync;
}

struct MatchFilesystemPathResult {
  1: optional EdenError error;
}

struct MatchFileSystemResponse {
  1: list<MatchFilesystemPathResult> results;
}

struct MatchFileSystemRequest {
  1: MountId mountPoint;
  2: list<PathString> paths;
}

struct ChangeOwnershipRequest {
  1: PathString mountPoint;
  2: i64 uid;
  3: i64 gid;
}

struct ChangeOwnershipResponse {}

struct GetBlockedFaultsRequest {
  1: string keyclass;
}

struct GetBlockedFaultsResponse {
  1: list<string> keyValues;
}

struct CheckoutProgressInfo {
  1: i64 updatedInodes;
  2: i64 totalInodes;
}

struct CheckoutNotInProgress {}

struct CheckoutProgressInfoRequest {
  1: PathString mountPoint;
}

union CheckoutProgressInfoResponse {
  1: CheckoutProgressInfo checkoutProgressInfo;
  2: CheckoutNotInProgress noProgress;
}

/*
 * Structs/Unions for changesSinceV2 API
 */

/*
 * Small change notification returned when invoking changesSinceV2.
 * Indicates that a new filesystem entry has been added to the
 * given mount point since the provided journal position.
 *
 * fileType - Dtype of added filesystem entry.
 * path - path (vector of bytes) of added filesystem entry.
 */
struct Added {
  1: Dtype fileType;
  3: PathString path;
}

/*
 * Small change notification returned when invoking changesSinceV2.
 * Indicates that an existing filesystem entry has been modified within
 * the given mount point since the provided journal position.
 *
 * fileType - Dtype of modified filesystem entry.
 * path - path (vector of bytes) of modified filesystem entry.
 */
struct Modified {
  1: Dtype fileType;
  3: PathString path;
}

/*
 * Small change notification returned when invoking changesSinceV2.
 * Indicates that an existing filesystem entry has been renamed within
 * the given mount point since the provided journal position.
 *
 * fileType - Dtype of renamed filesystem entry.
 * from - path (vector of bytes) the filesystem entry was previously located at.
 * to - path (vector of bytes) the filesystem entry was relocated to.
 */
struct Renamed {
  1: Dtype fileType;
  2: PathString from;
  3: PathString to;
}

/*
 * Small change notification returned when invoking changesSinceV2.
 * Indicates that an existing filesystem entry has been replaced within
 * the given mount point since the provided journal position.
 *
 * fileType - Dtype of replaced filesystem entry.
 * from - path (vector of bytes) the filesystem entry was previously located at.
 * to - path (vector of bytes) the filesystem entry was relocated over.
 */
struct Replaced {
  1: Dtype fileType;
  2: PathString from;
  3: PathString to;
}

/*
 * Small change notification returned when invoking changesSinceV2.
 * Indicates that an existing filesystem entry has been removed from
 * the given mount point since the provided journal position.
 *
 * fileType - Dtype of removed filesystem entry.
 * path - path (vector of bytes) of removed filesystem entry.
 */
struct Removed {
  1: Dtype fileType;
  3: PathString path;
}

/*
 * Change notification returned when invoking changesSinceV2.
 * Indicates that the given change is small in impact - affecting
 * one or two filesystem entries at most.
 */
union SmallChangeNotification {
  1: Added added;
  2: Modified modified;
  3: Renamed renamed;
  4: Replaced replaced;
  5: Removed removed;
}

/*
 * Large change notification returned when invoking changesSinceV2.
 * Indicates that an existing directory has been renamed within
 * the given mount point since the provided journal position.
 */
struct DirectoryRenamed {
  1: PathString from;
  2: PathString to;
}

/*
 * Large change notification returned when invoking changesSinceV2.
 * Indicates that a commit transition has occurred within the
 * given mount point since the provided journal position.
 */
struct CommitTransition {
  1: ThriftRootId from;
  2: ThriftRootId to;
}

/*
 * Large change notification returned when invoking changesSinceV2.
 * Indicates that EdenfS was unable to track changes within the given
 * mount point since the provided journal position. Callers should
 * treat all filesystem entries as changed.
 */
enum LostChangesReason {
  // Unknown reason.
  UNKNOWN = 0,
  // The given mount point was remounted (or EdenFS was restarted).
  EDENFS_REMOUNTED = 1,
  // EdenFS' journal was truncated.
  JOURNAL_TRUNCATED = 2,
  // There were too many change notifications to report to the caller.
  TOO_MANY_CHANGES = 3,
}

/*
 * Large change notification returned when invoking changesSinceV2.
 * Indicates that EdenFS was unable to provide the changes to the caller.
 */
struct LostChanges {
  1: LostChangesReason reason;
}

/*
 * Change notification returned when invoking changesSinceV2.
 * Indicates that the given change is large in impact - affecting
 * an unknown number of filesystem entries.
 */
union LargeChangeNotification {
  1: DirectoryRenamed directoryRenamed;
  2: CommitTransition commitTransition;
  3: LostChanges lostChanges;
}

/*
 * Changed returned when invoking changesSinceV2.
 * Contains a change that occurred within the given mount point
 * since the provided journal position.
 */
union ChangeNotification {
  1: SmallChangeNotification smallChange;
  2: LargeChangeNotification largeChange;
  3: StateChangeNotification stateChange;
}

struct StateEntered {
  1: string name;
}

struct StateLeft {
  1: string name;
}

union StateChangeNotification {
  1: StateEntered stateEntered;
  2: StateLeft stateLeft;
}

/**
 * Return value of the changesSinceV2 API
 *
 * toPosition - a new journal position that indicates the next change
 *   that will occur in the future. Should be used in the next call to
 *   changesSinceV2 go get the next list of changes.
 *
 * changes -  a list of all change notifications that have occurred in
 *   within the given mount point since the provided journal position.
 */
struct ChangesSinceV2Result {
  1: JournalPosition toPosition;
  2: list<ChangeNotification> changes;
}

struct PeekCurrentJournalPositionRequest {
  1: MountId mountId;
}

struct PeekCurrentJournalPositionResponse {
  1: JournalPosition position;
}

/**
 * Argument to changesSinceV2 API
 *
 * mountPoint - the EdenFS checkout to request changes about.
 *
 * fromPosition - the journal position used as the starting point to
 *   request changes since. Typically, fromPosition is the set to the
 *   toPosition value returned in ChangesSinceV2Result. However, for
 *   the initial invocation of changesSinceV2, the caller can obtain
 *   the current journal position by calling getCurrentJournalPosition.
 *
 * includeVCSRoots - optional flag indicating the VCS roots should be included
 *   in the returned results. By default, VCS roots will be excluded from
 *   results.
 *
 * includedRoots - optional list of roots to include in results. If not
 *   provided or an empty list, all roots will be included in results.
 *   Applied before roots are excluded - see excludedRoots.
 *
 * excludedRoots - optional list of roots to exclude from results. If not
 *   provided or an empty list, no roots will be excluded from results.
 *   Applied after roots are included - see includedRoots.
 *
 * includedSuffixes - optional list of suffixes to include in results. If not
 *   provided or an empty list, all suffixes will be included in results.
 *   Applied before suffixes are excluded - see excludedSuffixes.
 *
 * excludedSuffixes - optional list of suffixes to exclude from results. If not
 *   provided or an empty list, no suffixes will be excluded from results.
 *   Applied after suffixes are included - see includedSuffixes.
 */
struct ChangesSinceV2Params {
  1: PathString mountPoint;
  2: JournalPosition fromPosition;
  3: optional bool includeVCSRoots;
  4: optional list<PathString> includedRoots;
  5: optional list<PathString> excludedRoots;
  6: optional list<string> includedSuffixes;
  7: optional list<string> excludedSuffixes;
  8: optional PathString root;
  9: SyncBehavior sync;
  10: optional bool includeStateChanges;
}

/*
 * Return value of the startFileAccessMonitor API
 *
 * pid - The process ID for the started File Access Monitor(FAM) binary if started
 *   successfully.
 *
 * tmpOutputPath - The path of FAM output file
 */
struct StartFileAccessMonitorResult {
  1: pid_t pid;
  2: PathString tmpOutputPath;
}

/*
 * Return value of the stopFileAccessMonitor API
 *
 * tmpOutputPath - The path to the file which file access events are dumped to.
 *
 * specifiedOutputPath - If set, it tells the caller if a specified output path was provided.
 *
 * shouldUpload - It tells the caller if FAM is started with the request to upload the output file.
 */
struct StopFileAccessMonitorResult {
  1: PathString tmpOutputPath;
  2: PathString specifiedOutputPath;
  3: bool shouldUpload;
}

/**
 * Argument to startFileAccessMonitor API
 *
 * paths - A list of paths monitored by File Access Monitor(FAM).
 *
 * specifiedOutputPath - If provided, this is the destination where the file written
 *   by FAM will be moved to. This is only stored as part of the state of FAM. No
 *   writes will be made to this path.
 *
 * shouldUpload - It indicates if the output file should be uploaded. This is only
 *   stored as part of the state of FAM.
 */
struct StartFileAccessMonitorParams {
  1: list<PathString> paths;
  2: optional PathString specifiedOutputPath;
  3: bool shouldUpload;
}

struct SendNotificationResponse {}

struct SendNotificationRequest {
  1: string title;
  2: string description;
}

enum RedirectionType {
  BIND = 0,
  SYMLINK = 1,
  UNKNOWN = 2,
}

enum RedirectionState {
  // Matches the expectations of our configuration as far as we can tell
  MATCHES_CONFIGURATION = 0,
  // Something is mounted that we don't have a configuration for
  UNKNOWN_MOUNT = 1,
  // We expected the redirect to be mounted, but it isn't
  NOT_MOUNTED = 2,
  // We expected the redirect be a symlink, but it is not present
  SYMLINK_MISSING = 3,
  // The symlink is present, but it points to the wrong place
  SYMLINK_INCORRECT = 4,
}

struct Redirection {
  1: PathString repoPath;
  2: RedirectionType redirType;
  3: PathString source;
  4: RedirectionState state;
  /**
    * TODO: when is this not set (state == UNKNOWN)
    */
  5: optional PathString target;
}

struct ListRedirectionsRequest {
  1: MountId mount;
}

struct ListRedirectionsResponse {
  1: list<Redirection> redirections;
}

struct GetFileContentRequest {
  1: MountId mount;
  2: PathString filePath;
  3: SyncBehavior sync;
}

struct GetFileContentResponse {
  1: ScmBlobOrError blob;
}
/**
 * Result for a successful cancellation attempt.
 *
 * requestId - The request ID that was successfully cancelled.
 */
struct CancellationStatus {
  1: i64 requestId;
}

/**
 * Request parameters for the cancelRequests API.
 *
 * requestIds - The list of unique IDs of the requests to cancel. Each ID
 *   should correspond to an active or queued request in the system. Duplicate
 *   IDs in the list will result in multiple attempts to cancel the same request.
 */
struct CancelRequestsParams {
  1: list<i64> requestIds;
}

/**
 * Result for a single cancellation attempt.
 *
 * Either contains success information or an error describing why the
 * cancellation failed.
 */
union CancellationStatusOrError {
  1: CancellationStatus success;
  2: EdenError error;
}

/**
 * Response for the cancelRequests API.
 *
 * results - List of cancellation results corresponding to the input request IDs.
 *   The results are returned in the same order as the input requestIds from
 *   CancelRequestsParams. Each result contains the original request ID and the
 *   status of the cancellation attempt. The list will have the same length as
 *   the input requestIds list, with one result per input ID.
 */
struct CancelRequestsResponse {
  1: list<CancellationStatusOrError> results;
}

/**
 * Information about an active request.
 *
 * requestId - The unique ID of the request.
 * method - The name of the thrift method being called.
 * clientPid - The PID of the client that made the request.
 * cancelable - Whether this request can be cancelled.
 * processName - Process name/command line of the client process that initiated this request.
 *   Contains the full command line (with null-delimited arguments) from /proc/<pid>/cmdline.
 *   This field helps clients identify which command they intend to cancel.
 * startTimeNs - Timestamp when the request was first initiated (nanoseconds since epoch).
 *   This helps clients determine the age of the request for cancellation decisions.
 */
struct ActiveRequestInfo {
  1: i64 requestId;
  2: string method;
  3: pid_t clientPid;
  4: bool cancelable;
  5: optional string processName;
  6: optional i64 startTimeNs;
}

/**
 * Response for the getActiveRequests API.
 *
 * requests - List of currently active requests.
 */
struct GetActiveRequestsResponse {
  1: list<ActiveRequestInfo> requests;
}

service EdenService extends fb303_core.BaseService {
  list<MountInfo> listMounts() throws (1: EdenError ex);
  void mount(1: MountArgument info) throws (1: EdenError ex);
  void unmount(1: PathString mountPoint) throws (1: EdenError ex);
  void unmountV2(1: UnmountArgument unmountArgument) throws (1: EdenError ex);

  /**
   * Potentially check out the specified snapshot, reporting conflicts (and
   * possibly errors), as appropriate.
   *
   * If the checkoutMode is FORCE, the working directory will be forcibly
   * updated to the contents of the new snapshot, even if there were conflicts.
   * Conflicts will still be reported in the return value, but the files will be
   * updated to their new state.
   *
   * If the checkoutMode is NORMAL, files with conflicts will be left
   * unmodified. Files that are untracked in both the source and destination
   * snapshots are always left unchanged, even if force is true.
   *
   * If the checkoutMode is DRY_RUN, then no files are modified in the working
   * copy and the current snapshot does not change. However, potential conflicts
   * are still reported in the return value.
   *
   * On successful return from this function (unless it is a DRY_RUN), the mount
   * point will point to the new snapshot, even if some paths had conflicts or
   * errors. The caller is responsible for taking appropriate action to update
   * these paths as desired after checkOutRevision() returns.
   *
   * Note: this internally synchronize the working copy.
   */
  list<CheckoutConflict> checkOutRevision(
    1: PathString mountPoint,
    2: ThriftRootId snapshotHash,
    3: CheckoutMode checkoutMode,
    4: CheckOutRevisionParams params,
  ) throws (1: EdenError ex);

  /**
   * Given an Eden mount point returns progress for the checkOutRevision end
   * point. When a checkout is not in progress it returns CheckoutNotInProgress
   *
   * It errors out when no valid mountPoint is provided.
   */
  CheckoutProgressInfoResponse getCheckoutProgressInfo(
    1: CheckoutProgressInfoRequest params,
  ) throws (1: EdenError ex);

  /**
   * Reset the working directory's parent commits, without changing the working
   * directory contents.
   *
   * This operation is equivalent to `git reset --soft` or `hg reset --keep`
   */
  void resetParentCommits(
    1: PathString mountPoint,
    2: WorkingDirectoryParents parents,
    3: ResetParentCommitsParams params,
  ) throws (1: EdenError ex);

  /**
   * Gets information about the current snapshot (i.e. last checked out commit)
   * Currently, we only expose thethe current filter for the working copy. If
   * the working copy is not filtered then the returned filter will be none.
   */
  GetCurrentSnapshotInfoResponse getCurrentSnapshotInfo(
    1: GetCurrentSnapshotInfoRequest params,
  ) throws (1: EdenError ex);

  /**
   * Ensure that all inflight working copy modification have completed.
   *
   * On some platforms, EdenFS is processing working copy modifications
   * callbacks from the platform in an asynchronous manner, which means that by
   * the time a write/creat/mkdir/unlink/etc syscall returns from the kernel,
   * EdenFS may not have updated its internal state.
   *
   * Thus, an application making changes to the working copy and quickly
   * requesting EdenFS to perform an operation on it will race with EdenFS
   * updating its internal state and may thus get stale data.
   *
   * To avoid this, applications can call this method prior to issuing other
   * Thrift requests (such as getSHA1, globFiles, etc) to wait for EdenFS to
   * update its internal state. Applications that care about synchronizing
   * EdenFS up to a certain point in time are expected to call this once and
   * then issue all their thrift requests without synchronizing.
   *
   * As an alternative, applications may also set the SyncBehavior of a Thrift
   * method to a non-zero value to achieve the same result.
   *
   * Some Thrift methods are implicitly synchronizing, their documentation
   * will state it.
   */
  void synchronizeWorkingCopy(
    1: PathString mountPoint,
    2: SynchronizeWorkingCopyParams params,
  ) throws (1: EdenError ex);

  /**
   * For each path, returns an EdenError instead of the SHA-1 if any of the
   * following occur:
   * - path is the empty string.
   * - path identifies a non-existent file.
   * - path identifies something that is not an ordinary file (e.g., symlink
   *   or directory).
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<SHA1Result> getSHA1(
    1: PathString mountPoint,
    2: list<PathString> paths,
    3: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * For each path, returns an EdenError instead of the BLAKE3 if any of the
   * following occur:
   * - path is the empty string.
   * - path identifies a non-existent file.
   * - path identifies something that is not an ordinary file (e.g., symlink
   *   or directory).
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<Blake3Result> getBlake3(
    1: PathString mountPoint,
    2: list<PathString> paths,
    3: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * For each path, returns an EdenError instead of the DIGEST_HASH if any of the
   * following occur:
   * - directory is materialized (directory or child is/was modified since the
   *   last checkout operation).
   * - path identifies a non-existent file.
   * - path identifies something that is not an ordinary file or directory (e.g.,
   *   symlink or socket).
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<DigestHashResult> getDigestHash(
    1: PathString mountPoint,
    2: list<PathString> paths,
    3: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * On systems that support bind mounts, establish a bind mount within the
   * repo such that `mountPoint / repoPath` is redirected to `targetPath`.
   * If `repoPath` is already a bind mount managed by eden, this function
   * will throw an error.
   * If `repoPath` is not a directory then it will be created similar to
   * running `mkdir -p mountPoint/repoPath` and then the bind mount
   * will be established.
   * If `repoPath` exists and is not a directory, an error will be thrown.
   * If the bind mount cannot be set up, an error will be thrown.
   */
  void addBindMount(
    1: PathString mountPoint,
    2: PathString repoPath,
    3: PathString targetPath,
  ) throws (1: EdenError ex);

  /**
   * Removes the bind mount specified by `repoPath` from the set of managed
   * bind mounts.
   * If `repoPath` is not a bind mount managed by eden, this function
   * will throw an error.
   * If the bind mount cannot be removed, an error will be thrown.
   */
  void removeBindMount(
    1: PathString mountPoint,
    2: PathString repoPath,
  ) throws (1: EdenError ex);

  /** Returns the sequence position at the time the method is called.
   * Returns the instantaneous value of the journal sequence number.
   */
  JournalPosition getCurrentJournalPosition(1: PathString mountPoint) throws (
    1: EdenError ex,
  );

  /** Like getCurrentJournalPosition but does not mark the journal as observed.
   * Use this when you only need the current position and don't want to
   * affect subscriber notification coalescing.
   */
  PeekCurrentJournalPositionResponse peekCurrentJournalPosition(
    1: PeekCurrentJournalPositionRequest params,
  ) throws (1: EdenError ex);

  /** Returns the set of files (and dirs) that changed since a prior point.
   * If fromPosition.mountGeneration is mismatched with the current
   * mountGeneration, throws an EdenError with errorCode = ERANGE.
   * If the domain required by fromPosition goes past the Journal's memory,
   * throws an EdenError with errorCode = EDOM.
   * This indicates that eden cannot compute the delta for the requested
   * range.  The client will need to recompute a new baseline using
   * other available functions in EdenService.
   */
  FileDelta getFilesChangedSince(
    1: PathString mountPoint,
    2: JournalPosition fromPosition,
  ) throws (1: EdenError ex);

  /** Sets the memory limit on the journal such that the journal will forget
   * old data to keep itself under a certain estimated memory use.
   */
  void setJournalMemoryLimit(1: PathString mountPoint, 2: i64 limit) throws (
    1: EdenError ex,
  );

  /** Gets the memory limit on the journal
   */
  i64 getJournalMemoryLimit(1: PathString mountPoint) throws (1: EdenError ex);

  /** Forces the journal to flush, sending a truncated result to subscribers
   */
  void flushJournal(1: PathString mountPoint) throws (1: EdenError ex);

  /**
   * Returns the journal entries for the specified params. Useful for auditing
   * the changes that Eden has sent to Watchman. Note that the most recent
   * journal entries will be at the front of the list in
   * DebugGetRawJournalResponse.
   */
  DebugGetRawJournalResponse debugGetRawJournal(
    1: DebugGetRawJournalParams params,
  ) throws (1: EdenError ex);

  /**
   * Returns the subset of information about a list of paths that can
   * be determined from each's parent directory tree. For now, that
   * includes whether the entry exists and its dtype.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<EntryInformationOrError> getEntryInformation(
    1: PathString mountPoint,
    2: list<PathString> paths,
    3: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * Returns a subset of the stat() information for a list of paths.
   * The returned list of information corresponds to the input list of
   * paths; eg; result[0] holds the information for paths[0].
   * We only support returning the instantaneous information about
   * these paths, as we cannot answer with historical information about
   * files in the overlay.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<FileInformationOrError> getFileInformation(
    1: PathString mountPoint,
    2: list<PathString> paths,
    3: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * Returns the requested file attributes for the provided list of files.
   * The result maps the files to attribute results which may be an EdenError
   * or a FileAttributeDataV2 struct.
   *
   * This does not assume that all the inputs are regular files. This endpoint
   * will attempt to return attributes for any type of file (directories
   * included) unless instructed otherwise. Note that some attributes are not
   * currently supported, like sha1 and size for directories and symlinks. At
   * some point EdenFS may be able to support such attributes.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specifies a 0 timeout. See the documentation for both of
   * these for more details.
   */
  GetAttributesFromFilesResultV2 getAttributesFromFilesV2(
    1: GetAttributesFromFilesParams params,
  ) throws (1: EdenError ex);

  /**
   * Returns the requested file attributes for each of the entries in the
   * specified directories. . and .. are not included in the entry list.
   *
   * sha1 and size are not available for directories and symlinks, error values
   * will be returned for those attributes for entries of those types.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  ReaddirResult readdir(1: ReaddirParams params) throws (1: EdenError ex);

  /**
   * DEPRECATED: Use globFiles().
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, see the
   * documentation for that method.
   */
  list<PathString> glob(
    1: PathString mountPoint,
    2: list<string> globs,
  ) throws (1: EdenError ex);

  /**
   * Returns a list of files that match the GlobParams, notably,
   * the list of glob patterns.
   * There are no duplicate values in the result.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  Glob globFiles(1: GlobParams params) throws (1: EdenError ex);

  /**
   * DEPRECATED: use prefetchFilesV2
   *
   * Has the same behavior as globFiles, but should be called in the case of a prefetch.
   * This request could be deprioritized since it will be assumed that this call is used
   * for optimization and the result not relied on for operations. This command does not
   * return the list of prefetched files.
   */
  @thrift.Priority{level = thrift.RpcPriority.BEST_EFFORT}
  void prefetchFiles(1: PrefetchParams params) throws (1: EdenError ex);

  /**
   * Has the same behavior as globFiles, but should be called in the case of a prefetch.
   * This call is used when prefetching instead of globbing, to allow for different behaviors.
   * This command returns a PrefetchResult, which contains the list of prefetched files.
   * If returnPrefetchedFiles is true, this command will return the prefetched files.
   */
  @thrift.Priority{level = thrift.RpcPriority.BEST_EFFORT}
  PrefetchResult prefetchFilesV2(1: PrefetchParams params) throws (
    1: EdenError ex,
  );

  /**
   * Query the progress of a page cache preload operation.
   * The operationId is returned from prefetchFilesV2 when preload=true
   * and preloadProgress=true. Returns found=false for ids the daemon no
   * longer (or never) tracked; see PreloadProgressResponse.
   */
  PreloadProgressResponse queryPreloadProgress(
    1: PreloadProgressRequest request,
  ) throws (1: EdenError ex);

  /**
   * DEPRECATED: predictive prefetching has been removed. This method is a
   * no-op that always returns an empty Glob.
   */
  Glob predictiveGlobFiles(1: GlobParams params) throws (1: EdenError ex);

  /**
   * Chowns all files in the requested mount to the requested uid and gid
   *
   * DEPRECATED: Prefer using ChangeOwnership in new code.  Callers may still
   * need to fall back to chown() if talking to an older edenfs daemon
   * that does not support ChangeOwnership() yet.
   */
  void chown(1: PathString mountPoint, 2: i32 uid, 3: i32 gid);

  /**
   * Changes ownership all files in the requested mount to the requested uid and gid
   */
  ChangeOwnershipResponse changeOwnership(
    1: ChangeOwnershipRequest request,
  ) throws (1: EdenError ex);

  /**
   * Return the list of files that are different from the specified source
   * control commit.
   *
   * Note: this internally synchronize the working copy and thus the returned
   * data is guaranteed to return be the set of files that changed prior to
   * calling this method.
   */
  GetScmStatusResult getScmStatusV2(1: GetScmStatusParams params) throws (
    1: EdenError ex,
  );

  /**
   * Computes the status between two specified revisions.
   * This does not care about the state of the working copy.
   *
   * This is used by Watchman and which will be deprecated once EdenFS provides
   * an API to get the set of files changed between 2 journal clocks.
   */
  ScmStatus getScmStatusBetweenRevisions(
    1: PathString mountPoint,
    2: ThriftRootId oldHash,
    3: ThriftRootId newHash,
  ) throws (1: EdenError ex);

  /**
   * When eden doctor or another tool notices that EdenFS is out of sync with
   * the filesystem this API can be used to poke EdenFS into noticing the file
   * change.
   */
  MatchFileSystemResponse matchFilesystem(
    1: MatchFileSystemRequest params,
  ) throws (1: EdenError ex);

  //////// Administrative APIs ////////

  /**
   * Returns information about the running process, including pid and command
   * line.
   */
  @thrift.Priority{level = thrift.RpcPriority.IMPORTANT}
  DaemonInfo getDaemonInfo() throws (1: EdenError ex);

  /**
  * Returns information about the privhelper process, including accessibility.
  */
  PrivHelperInfo checkPrivHelper() throws (1: EdenError ex);

  /**
   * DEPRECATED
   *
   * Returns the pid of the running edenfs daemon. New code should call
   * getDaemonInfo instead. This method exists for Thrift clients that
   * predate getDaemonInfo, such as older versions of the CLI.
   */
  i64 getPid() throws (1: EdenError ex);

  /**
   * Ask the server to shutdown and provide it some context for its logs
   */
  void initiateShutdown(1: string reason) throws (1: EdenError ex);

  /**
   * Get the current configuration settings
   */
  eden_config.EdenConfigData getConfig(1: GetConfigParams params) throws (
    1: EdenError ex,
  );

  /**
   * Ask eden to reload its configuration data from disk.
   */
  void reloadConfig() throws (1: EdenError ex);

  //////// Debugging APIs ////////

  /**
   * DEPRECATED: Use debugGetTree().
   * TODO: remove this API after 07/01/2024
   *
   * Get the contents of a source control Tree.
   *
   * This can be used to confirm if eden's LocalStore contains information
   * for the tree, and that the information is correct.
   *
   * If localStoreOnly is true, the data is loaded directly from the
   * LocalStore, and an error will be raised if it is not already present in
   * the LocalStore.  If localStoreOnly is false, the data may be retrieved
   * from the BackingStore if it is not already present in the LocalStore.
   */
  list<ScmTreeEntry> debugGetScmTree(
    1: PathString mountPoint,
    2: ThriftObjectId id,
    3: bool localStoreOnly,
  ) throws (1: EdenError ex);

  DebugGetScmTreeResponse debugGetTree(
    1: DebugGetScmTreeRequest request,
  ) throws (1: EdenError ex);

  /**
   * Get the contents of a source control Blob.
   *
   * The origins field can control where to check for the blob. This will
   * attempt to fetch the blob from all locations and return the blob contents
   * from each of the locations.
   */
  DebugGetScmBlobResponse debugGetBlob(
    1: DebugGetScmBlobRequest request,
  ) throws (1: EdenError ex);

  /**
   * Get the metadata about a source control Blob.
   *
   * This retrieves the metadata about a source control Blob.  This returns
   * the size and contents SHA1 of the blob, which eden stores separately from
   * the blob itself.  This can also be a useful alternative to
   * debugGetBlob() when getting data about extremely large blobs.
   *
   * The origins field can control where to check for the blob. This will
   * attempt to fetch the blob from all locations and return the blob contents
   * from each of the locations.
   */
  DebugGetBlobMetadataResponse debugGetBlobMetadata(
    1: DebugGetBlobMetadataRequest request,
  ) throws (1: EdenError ex);

  /**
   * Get status about inodes with allocated inode numbers.
   *
   * This returns details about all previously-observed inode objects under the
   * given path.
   *
   * If the path argument is the empty string data will be returned about all
   * inodes in the entire mount point.  Otherwise the path argument should
   * refer to a subdirectory, and data will be returned for all inodes under
   * the specified subdirectory.
   *
   * The rename lock is not held while gathering this information, so the path
   * name information returned may not always be internally consistent.  If
   * renames were taking place while gathering the data, some inodes may show
   * up under multiple parents.  It's also possible that we may miss some
   * inodes during the tree walk if they were renamed from a directory that was
   * not yet walked into a directory that has already been walked.
   *
   * This API cannot return data about inodes that have been unlinked but still
   * have outstanding references.
   *
   * Note: may return stale data if synchronizeWorkingCopy isn't called, and if
   * the SyncBehavior specify a 0 timeout. see the documentation for both of
   * these for more details.
   */
  list<TreeInodeDebugInfo> debugInodeStatus(
    1: PathString mountPoint,
    2: PathString path,
    3: i64 flags,
    4: SyncBehavior sync,
  ) throws (1: EdenError ex);

  /**
   * Get the list of outstanding fuse requests
   *
   * This will return the list of FuseCall structure containing the data from
   * fuse_in_header.
   */
  list<FuseCall> debugOutstandingFuseCalls(1: PathString mountPoint);

  /**
   * Get the list of outstanding NFS requests
   *
   * This will return the list of NfsCall structure containing the data from the RPC request.
   */
  list<NfsCall> debugOutstandingNfsCalls(1: PathString mountPoint);

  /**
   * Get the list of outstanding ProjectedFS requests
   *
   * This will return the list of PrjfsCall structure containing the data from
   * the PRJ_CALLBACK_DATA.
   */
  list<PrjfsCall> debugOutstandingPrjfsCalls(1: PathString mountPoint);

  /**
   * Get the list of outstanding Thrift requests
   */
  list<ThriftRequestMetadata> debugOutstandingThriftRequests();

  /**
   * Get the list of outstanding file download events from source control servers
   */
  list<HgEvent> debugOutstandingHgEvents(1: PathString mountPoint);

  /**
   * Start recording performance metrics such as files read
   *
   * This will return a structure containing unique id identifying this recording.
   */
  ActivityRecorderResult debugStartRecordingActivity(
    1: PathString mountPoint,
    2: PathString outputDir,
  );

  /**
   * Stop the recording identified by unique
   *
   * This will return a structure containing unique id identifying this recording
   * and, if the recording is successfully stopped, the output file path.
   */
  ActivityRecorderResult debugStopRecordingActivity(
    1: PathString mountPoint,
    2: i64 unique,
  );

  /**
   * Get the list of ongoing activity recordings
   *
   * This will return the list of ActivityRecorderResult structure
   * containing the id and output file path.
   */
  ListActivityRecordingsResult debugListActivityRecordings(
    1: PathString mountPoint,
  );

  /**
   * Get the InodePathDebugInfo for the inode that corresponds to the given
   * inode number. This provides the path for the inode and also indicates
   * whether the inode is currently loaded or not. Requires that the Eden
   * mountPoint be specified.
   */
  InodePathDebugInfo debugGetInodePath(
    1: PathString mountPoint,
    2: i64 inodeNumber,
  ) throws (1: EdenError ex);

  /**
   * Clear pidFetchCounts_ in ObjectStore to start a new recording of process
   * fetch counts.
   */
  void clearFetchCounts() throws (1: EdenError ex);
  void clearFetchCountsByMount(1: PathString mountPath) throws (
    1: EdenError ex,
  );

  /**
   * Queries all of the live Eden mounts for the processes that accessed FUSE
   * over the last `duration` seconds.
   *
   * Note that eden only maintains a few seconds worth of accesses.
   */
  GetAccessCountsResult getAccessCounts(1: i64 duration) throws (
    1: EdenError ex,
  );

  /**
   * Start recording paths of the files fetched from the backing store.
   *
   * Note that using this call twice will not clear the data and start a new
   * recording.
   */
  void startRecordingBackingStoreFetch() throws (1: EdenError ex);

  /**
   * Stop recording paths of the files fetched from the backing store.
   *
   * Note that using this call will return and clear the previously
   * collected data.
   */
  GetFetchedFilesResult stopRecordingBackingStoreFetch() throws (
    1: EdenError ex,
  );

  /**
   * Requests EdenFS to drop all pending backing store fetches.
   * Returns the number of requests EdenFS dropped from the queue.
   */
  i64 debugDropAllPendingRequests() throws (1: EdenError ex);

  /**
  * Unloads unused Inodes from a directory inside a mountPoint whose last
  * access time is older than the specified age.
  *
  * The age parameter is a relative time to be subtracted from the current
  * (wall clock) time.
  */
  i64 unloadInodeForPath(
    1: PathString mountPoint,
    2: PathString path,
    3: TimeSpec age,
  ) throws (1: EdenError ex);

  /**
   * Garbage collect the repository by invalidating all the files/directories
   * below the passed in path who haven't been accessed since the given age.
   *
   * TODO: Rewrite as a streaming API once eden doctor is in Rust.
   */
  DebugInvalidateResponse debugInvalidateNonMaterialized(
    1: DebugInvalidateRequest params,
  ) throws (1: EdenError ex);

  /**
   * Flush all thread-local stats to the main ServiceData object.
   *
   * Thread-local counters are normally flushed to the main ServiceData once
   * a second.  flushStatsNow() can be used to flush thread-local counters on
   * demand, in addition to the normal once-a-second flush.
   *
   * This is mainly useful for unit and integration tests that want to ensure
   * they see up-to-date counter information without waiting for the normal
   * flush interval.
   */
  void flushStatsNow() throws (1: EdenError ex);

  /**
  * Invalidate kernel cache for inode.
  */
  void invalidateKernelInodeCache(
    1: PathString mountPoint,
    2: PathString path,
  ) throws (1: EdenError ex);

  /**
   * Gets the number of inodes unloaded by periodic job on an EdenMount.
   */
  InternalStats getStatInfo(1: GetStatInfoParams params) throws (
    1: EdenError ex,
  );

  void enableTracing();
  void disableTracing();
  list<TracePoint> getTracePoints();

  /**
   * Gets a list of thrift request events stored on the thrift server's ActivityBuffer.
   * Used for retroactive debugging by the `eden trace thrift --retroactive` command.
   */
  GetRetroactiveThriftRequestEventsResult getRetroactiveThriftRequestEvents() throws (
    1: EdenError ex,
  );

  /**
   * Gets a list of Sapling events stored in Eden's Sapling ActivityBuffer. Used for
   * retroactive debugging by the `eden trace sl --retroactive` command.
   */
  GetRetroactiveHgEventsResult getRetroactiveHgEvents(
    1: GetRetroactiveHgEventsParams params,
  ) throws (1: EdenError ex);

  /**
   * Gets a list of inode events stored in a specified EdenMount's
   * ActivityBuffer. Used for retroactive debugging by the `eden trace inode
   * --retroactive` command. Supports inode load and materialization events.
   */
  GetRetroactiveInodeEventsResult getRetroactiveInodeEvents(
    1: GetRetroactiveInodeEventsParams params,
  ) throws (1: EdenError ex);

  /**
   * Configure a new fault in Eden's fault injection framework.
   *
   * This throws an exception if the fault injection framework was not enabled
   * when edenfs was started.
   */
  void injectFault(1: FaultDefinition fault) throws (1: EdenError ex);

  /**
   * Remove a fault previously defined with injectFault()
   *
   * Returns true if a matching fault was found and remove, and false
   * if no matching fault was found.
   */
  bool removeFault(1: RemoveFaultArg fault) throws (1: EdenError ex);

  /**
   * Unblock fault injection checks pending on a block fault.
   *
   * Returns the number of pending calls that were unblocked
   */
  i64 unblockFault(1: UnblockFaultArg info) throws (1: EdenError ex);

  GetBlockedFaultsResponse getBlockedFaults(
    1: GetBlockedFaultsRequest request,
  ) throws (1: EdenError ex);

  /**
   * Directly load a BackingStore object identified by id at the given path.
   *
   * If any file or directory name conflict, the behavior is same with Checkout
   * This method is thread safe.
   */
  SetPathObjectIdResult setPathObjectId(
    1: SetPathObjectIdParams params,
  ) throws (1: EdenError ex);

  /**
   * Functionally same as rm -r command but more efficient since rm command would
   * load every subtree before unlinking it.
   */
  void removeRecursively(1: RemoveRecursivelyParams params) throws (
    1: EdenError ex,
  );

  /**
   * Eagerly materialize a list of paths, which can improve the latency of random reads.
   * If the path is a file, materialize the file.
   * If the path is a directory, recursively materialize its children.
   * If the path is a symlink, materialize its target if followSymlink option is set.
   *
   * This method should be used carefully, as EdenFS will copy the file contents into the overlay,
   * consuming disk space. Also, materialized files slow down checkout and status operations.
   */
  void ensureMaterialized(1: EnsureMaterializedParams params) throws (
    1: EdenError ex,
  );

  /**
   * Returns a list of change notifications along with a new journal position for a given mount
   * point since a provided journal position.
   *
   * This does not resolve expensive operations like moving a directory or changing
   * commits. Callers must query Sapling to evaluate those potentially expensive operations.
   */
  ChangesSinceV2Result changesSinceV2(1: ChangesSinceV2Params params) throws (
    1: EdenError ex,
  );

  StartFileAccessMonitorResult startFileAccessMonitor(
    1: StartFileAccessMonitorParams params,
  ) throws (1: EdenError ex);

  StopFileAccessMonitorResult stopFileAccessMonitor() throws (1: EdenError ex);

  /**
  * Ask the server to send a notification to the user via the Notifier.
  *
  * Note that only Windows has a Notifier implementation. This is a no-op on other platforms.
  */
  SendNotificationResponse sendNotification(
    1: SendNotificationRequest request,
  ) throws (1: EdenError ex);

  ListRedirectionsResponse listRedirections(
    1: ListRedirectionsRequest request,
  ) throws (1: EdenError ex);

  /**
   * Returns a blobOrError for the given path.
   * Caller needs to ensure the path goes to a valid file.
   *
   * Note that the non-streaming API CANNOT get blobs larger than 2GB,
   * due to thrift size limits.
   */
  GetFileContentResponse getFileContent(
    1: GetFileContentRequest request,
  ) throws (1: EdenError ex);

  /**
   * Cancels requests with the given request IDs.
   *
   * This method allows clients to cancel ongoing requests by providing
   * the unique request IDs. For each request ID, if the request is found
   * and can be cancelled, the cancellation is triggered. The response
   * contains the status of each cancellation attempt.
   */
  CancelRequestsResponse cancelRequests(1: CancelRequestsParams params) throws (
    1: EdenError ex,
  );

  /**
   * Gets a list of currently active requests.
   *
   * This method returns information about all currently active Thrift requests,
   * including their request IDs, method names, client PIDs, and whether they
   * can be cancelled.
   */
  GetActiveRequestsResponse getActiveRequests() throws (1: EdenError ex);

  /**
   * Debug endpoint to test the structured error logging pipeline end-to-end.
   * Throws a test exception, catches it, and logs it via ErrorLogger to
   * edenfs_errors through XplatLogger. Returns true if the event was logged,
   * false if error logging is not configured or disabled.
   * Use: eden debug thrift debugLogError
   */
  bool debugLogError() throws (1: EdenError ex);
}
