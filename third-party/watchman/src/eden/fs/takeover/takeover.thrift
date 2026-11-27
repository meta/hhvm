/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

namespace cpp2 facebook.eden

include "thrift/annotation/cpp.thrift"
include "thrift/annotation/thrift.thrift"

@thrift.AllowLegacyMissingUris
package;

@cpp.Type{name = "uint64_t"}
typedef i64 ui64

// A list of takeover data serialization versions that the client supports
struct TakeoverVersionQuery {
  // The set of versions supported by the client. Only version 7 is accepted;
  // the field is kept so that a peer that predates capability matching can
  // report the mismatch.
  1: set<i32> versions;
  // The set of capabilities supported by the client.
  2: ui64 capabilities;
  // The client asks the server to cancel the Thrift requests it is serving
  // instead of refusing the takeover while one of them has run for long.
  3: optional bool force;
  // The client asks the server to refuse the takeover while it has served a
  // checkout or a mount recently, as an unattended restart does, on top of
  // refusing while a request is in flight.
  4: optional bool onlyIfIdle;
}

struct SerializedInodeMapEntry {
  1: i64 inodeNumber;
  2: i64 parentInode;
  3: string name;
  4: bool isUnlinked;
  5: i64 numFsReferences;
  // If unset, the inode is materialized.
  // LEGACY: if the empty string, the inode is materialized. In the future,
  // the semantics will change here, as a BackingStore might use the empty
  // string as a valid ID.
  6: optional string hash;
  7: i32 mode;
  // EdenTimestamp persistent wire encoding.
  8: optional ui64 lastFsRequestTime;
}

struct SerializedInodeMap {
  2: list<SerializedInodeMapEntry> unloadedInodes;
}

struct SerializedFileHandleMap {}

// A FileChangeJournalDelta.
struct SerializedFileChangeDelta {
  1: ui64 sequenceID;
  2: ui64 earliestSequenceID;
  // A steady_clock time, which every process on the machine shares.
  3: i64 timeNanos;
  // Each path is only set when the delta has it.
  4: optional binary path1;
  5: bool path1ExistedBefore;
  6: bool path1ExistedAfter;
  7: optional binary path2;
  8: bool path2ExistedBefore;
  9: bool path2ExistedAfter;
  10: i32 dtype;
}

// A RootUpdateJournalDelta.
struct SerializedRootUpdateDelta {
  1: ui64 sequenceID;
  2: ui64 earliestSequenceID;
  3: i64 timeNanos;
  4: binary fromRoot;
  5: list<binary> uncleanPaths;
}

// The newest part of a mount's journal, with the mount generation its
// positions belong to, so that the journal positions clients got from the old
// process stay valid in the new one.
struct SerializedJournal {
  1: ui64 mountGeneration;
  2: ui64 nextSequence;
  3: binary currentRoot;
  // Both lists are ordered from oldest to newest.
  4: list<SerializedFileChangeDelta> fileChangeDeltas;
  5: list<SerializedRootUpdateDelta> rootUpdateDeltas;
}

// Mount protocol to use for a mount that we are taking over.
enum TakeoverMountProtocol {
  UNKNOWN = 0,
  FUSE = 1,
  NFS = 2,
}

struct SerializedMountInfo {
  1: string mountPath;
  2: string stateDirectory;
  // TODO: remove this field, it is no longer used
  3: list<string> bindMountPaths;
  // This binary blob is really a fuse_init_out instance.
  // We don't transcribe that into thrift because the layout
  // is system dependent and happens to be flat and thus is
  // suitable for reinterpret_cast to be used upon it to
  // access the struct once we've moved it across the process
  // boundary.  Note that takeover is always local to the same
  // machine and thus has the same endianness.
  // This will be left empty for NFS mounts.
  4: binary connInfo; // fuse_init_out

  // Removed, do not use 5
  // 5: SerializedFileHandleMap fileHandleMap,

  6: SerializedInodeMap inodeMap;

  7: TakeoverMountProtocol mountProtocol = TakeoverMountProtocol.UNKNOWN;
}

enum FileDescriptorType {
  LOCK_FILE = 0,
  THRIFT_SOCKET = 1,
  MOUNTD_SOCKET = 2,
}

struct SerializedTakeoverInfo {
  1: list<SerializedMountInfo> mounts;
  2: list<FileDescriptorType> fileDescriptors;
}

// This is the highlevel structure we use to send takeover data between the
// client and server.
union SerializedTakeoverResult {
  1: SerializedTakeoverInfo takeoverData;
  2: string errorReason;
}
