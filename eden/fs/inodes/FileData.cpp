/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "FileData.h"

#include <folly/Exception.h>
#include <folly/FileUtil.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <openssl/sha.h>
#include "Overlay.h"
#include "eden/fs/inodes/EdenMount.h"
#include "eden/fs/inodes/FileInode.h"
#include "eden/fs/inodes/Overlay.h"
#include "eden/fs/model/Blob.h"
#include "eden/fs/model/Hash.h"
#include "eden/fs/store/ObjectStore.h"
#include "eden/fuse/BufVec.h"
#include "eden/fuse/MountPoint.h"
#include "eden/fuse/fuse_headers.h"
#include "eden/utils/XAttr.h"

using folly::ByteRange;
using folly::checkUnixError;
using folly::Future;
using folly::makeFuture;
using folly::Unit;

namespace facebook {
namespace eden {

FileData::FileData(FileInode* inode, const folly::Optional<Hash>& hash)
    : inode_(inode) {
  // The rest of the FileData code assumes that we always have file_ if
  // this is a materialized file.
  if (!hash.hasValue()) {
    auto filePath = inode_->getLocalPath();
    file_ = folly::File(filePath.c_str(), O_RDWR | O_NOFOLLOW, 0600);
  }
}

FileData::FileData(FileInode* inode, folly::File&& file)
    : inode_(inode), file_(std::move(file)) {}

// Conditionally updates target with either the value provided by
// the caller, or with the current time value, depending on the value
// of the flags in to_set.  Valid flag values are defined in fuse_lowlevel.h
// and have symbolic names matching FUSE_SET_*.
// useAttrFlag is the bitmask that indicates whether we should use the value
// from wantedTimeSpec.  useNowFlag is the bitmask that indicates whether we
// should use the current time instead.
// If neither flag is present, we will preserve the current value in target.
static void resolveTimeForSetAttr(
    struct timespec& target,
    int to_set,
    int useAttrFlag,
    int useNowFlag,
    const struct timespec& wantedTimeSpec) {
  if (to_set & useAttrFlag) {
    target = wantedTimeSpec;
  } else if (to_set & useNowFlag) {
    clock_gettime(CLOCK_REALTIME, &target);
  }
}

// Valid values for to_set are found in fuse_lowlevel.h and have symbolic
// names matching FUSE_SET_*.
struct stat FileData::setAttr(const struct stat& attr, int to_set) {
  auto state = inode_->state_.wlock();

  CHECK(file_) << "MUST have a materialized file at this point";

  // We most likely need the current information to apply the requested
  // changes below, so just fetch it here first.
  struct stat currentStat;
  checkUnixError(fstat(file_.fd(), &currentStat));

  if (to_set & FUSE_SET_ATTR_SIZE) {
    checkUnixError(ftruncate(file_.fd(), attr.st_size));
  }

  if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
    if ((to_set & FUSE_SET_ATTR_UID && attr.st_uid != currentStat.st_uid) ||
        (to_set & FUSE_SET_ATTR_GID && attr.st_gid != currentStat.st_gid)) {
      folly::throwSystemErrorExplicit(
          EACCES, "changing the owner/group is not supported");
    }

    // Otherwise: there is no change
  }

  if (to_set & FUSE_SET_ATTR_MODE) {
    // The mode data is stored only in inode_->state_.
    // (We don't set mode bits on the overlay file as that may incorrectly
    // prevent us from reading or writing the overlay data).
    // Make sure we preserve the file type bits, and only update permissions.
    state->mode = (state->mode & S_IFMT) | (07777 & attr.st_mode);
  }

  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME |
                FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW)) {
    // Changing various time components.
    // Element 0 is the atime, element 1 is the mtime.
    struct timespec times[2] = {currentStat.st_atim, currentStat.st_mtim};

    resolveTimeForSetAttr(
        times[0],
        to_set,
        FUSE_SET_ATTR_ATIME,
        FUSE_SET_ATTR_ATIME_NOW,
        attr.st_atim);

    resolveTimeForSetAttr(
        times[1],
        to_set,
        FUSE_SET_ATTR_MTIME,
        FUSE_SET_ATTR_MTIME_NOW,
        attr.st_mtim);

    checkUnixError(futimens(file_.fd(), times));
  }

  // We need to return the now-current stat information for this file.
  struct stat returnedStat;
  checkUnixError(fstat(file_.fd(), &returnedStat));
  returnedStat.st_mode = state->mode;

  return returnedStat;
}

struct stat FileData::stat() {
  auto st = inode_->getMount()->getMountPoint()->initStatData();
  st.st_nlink = 1;

  auto state = inode_->state_.rlock();

  if (file_) {
    // stat() the overlay file.
    //
    // TODO: We need to get timestamps accurately here.
    // The timestamps on the underlying file are not correct, because we keep
    // file_ open for a long time, and do not close it when FUSE file handles
    // close.  (Timestamps are typically only updated on close operations.)
    // This results our reported timestamps not changing correctly after the
    // file is changed through FUSE APIs.
    //
    // We probably should update the overlay file to include a header,
    // so we can store the atime, mtime, and ctime in the header data.
    // Otherwise we won't be able to report the ctime accurately if we just
    // keep using the overlay file timestamps.
    checkUnixError(fstat(file_.fd(), &st));
    st.st_mode = state->mode;
    st.st_rdev = state->rdev;
    return st;
  }

  CHECK(blob_);
  st.st_mode = state->mode;

  auto buf = blob_->getContents();
  st.st_size = buf.computeChainDataLength();

  // Report atime, mtime, and ctime as the time when we first loaded this
  // FileInode.  It hasn't been materialized yet, so this is a reasonble time
  // to use.  Once it is materialized we use the timestamps on the underlying
  // overlay file, which the kernel keeps up-to-date.
  auto epochTime = state->creationTime.time_since_epoch();
  auto epochSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(epochTime);
  st.st_atime = epochSeconds.count();
  st.st_mtime = epochSeconds.count();
  st.st_ctime = epochSeconds.count();
#if defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || \
    _POSIX_C_SOURCE >= 200809L || _XOPEN_SOURCE >= 700
  auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
      epochTime - epochSeconds);
  st.st_atim.tv_nsec = nsec.count();
  st.st_mtim.tv_nsec = nsec.count();
  st.st_ctim.tv_nsec = nsec.count();
#endif

  // NOTE: we don't set rdev to anything special here because we
  // don't support committing special device nodes.

  return st;
}

void FileData::flush(uint64_t /* lock_owner */) {
  // We have no write buffers, so there is nothing for us to flush,
  // but let's take this opportunity to update the sha1 attribute.
  auto state = inode_->state_.wlock();
  if (file_ && !sha1Valid_) {
    recomputeAndStoreSha1(state);
  }
}

void FileData::fsync(bool datasync) {
  auto state = inode_->state_.wlock();
  if (!file_) {
    // If we don't have an overlay file then we have nothing to sync.
    return;
  }

  auto res =
#ifndef __APPLE__
      datasync ? ::fdatasync(file_.fd()) :
#endif
               ::fsync(file_.fd());
  checkUnixError(res);

  // let's take this opportunity to update the sha1 attribute.
  if (!sha1Valid_) {
    recomputeAndStoreSha1(state);
  }
}

std::unique_ptr<folly::IOBuf> FileData::readIntoBuffer(size_t size, off_t off) {
  auto state = inode_->state_.rlock();

  if (file_) {
    auto buf = folly::IOBuf::createCombined(size);
    auto res = ::pread(file_.fd(), buf->writableBuffer(), size, off);
    checkUnixError(res);
    buf->append(res);
    return buf;
  }

  auto buf = blob_->getContents();
  folly::io::Cursor cursor(&buf);

  if (!cursor.canAdvance(off)) {
    // Seek beyond EOF.  Return an empty result.
    return folly::IOBuf::wrapBuffer("", 0);
  }

  cursor.skip(off);

  std::unique_ptr<folly::IOBuf> result;
  cursor.cloneAtMost(result, size);
  return result;
}

std::string FileData::readAll() {
  auto state = inode_->state_.rlock();
  if (file_) {
    std::string result;
    auto rc = lseek(file_.fd(), 0, SEEK_SET);
    folly::checkUnixError(rc, "unable to seek in materialized FileData");
    folly::readFile(file_.fd(), result);
    return result;
  }

  const auto& contentsBuf = blob_->getContents();
  folly::io::Cursor cursor(&contentsBuf);
  return cursor.readFixedString(contentsBuf.computeChainDataLength());
}

fusell::BufVec FileData::read(size_t size, off_t off) {
  auto buf = readIntoBuffer(size, off);
  return fusell::BufVec(std::move(buf));
}

size_t FileData::write(fusell::BufVec&& buf, off_t off) {
  auto state = inode_->state_.wlock();
  if (!file_) {
    // Not open for write
    folly::throwSystemErrorExplicit(EINVAL);
  }

  sha1Valid_ = false;
  auto vec = buf.getIov();
  auto xfer = ::pwritev(file_.fd(), vec.data(), vec.size(), off);
  checkUnixError(xfer);
  return xfer;
}

size_t FileData::write(folly::StringPiece data, off_t off) {
  auto state = inode_->state_.wlock();
  if (!file_) {
    // Not open for write
    folly::throwSystemErrorExplicit(EINVAL);
  }

  sha1Valid_ = false;
  auto xfer = ::pwrite(file_.fd(), data.data(), data.size(), off);
  checkUnixError(xfer);
  return xfer;
}

Future<Unit> FileData::ensureDataLoaded() {
  auto state = inode_->state_.wlock();

  if (!state->hash.hasValue()) {
    // We should always have the file open if we are materialized.
    CHECK(file_);
    return makeFuture();
  }

  if (blob_) {
    DCHECK_EQ(blob_->getHash(), state->hash.value());
    return makeFuture();
  }

  // Load the blob data.
  //
  // TODO: We really should use a Future-based API for this rather than
  // blocking until the load completes.  However, for that to work we will need
  // to add some extra data tracking whether or not we are already in the
  // process of loading the data.  We need to avoid multiple threads all trying
  // to load the data at the same time.
  //
  // For now doing a blocking load with the inode_->state_ lock held ensures
  // that only one thread can load the data at a time.  It's pretty unfortunate
  // to block with the lock held, though :-(
  blob_ = getObjectStore()->getBlob(state->hash.value());
  return makeFuture();
}

Future<Unit> FileData::materializeForWrite(int openFlags) {
  auto state = inode_->state_.wlock();

  // If we already have a materialized overlay file then we don't
  // need to do much
  if (file_) {
    CHECK(!state->hash.hasValue());
    if ((openFlags & O_TRUNC) != 0) {
      // truncating a file that we already have open
      sha1Valid_ = false;
      checkUnixError(ftruncate(file_.fd(), 0));
      auto emptySha1 = Hash::sha1(ByteRange{});
      storeSha1(state, emptySha1);
    }
    return makeFuture();
  }

  // We must not be materialized yet
  CHECK(state->hash.hasValue());

  Hash sha1;
  auto filePath = inode_->getLocalPath();
  if ((openFlags & O_TRUNC) != 0) {
    file_ = folly::File(filePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    sha1 = Hash::sha1(ByteRange{});
  } else {
    if (!blob_) {
      // TODO: Load the blob using the non-blocking Future APIs.
      // However, just as in ensureDataLoaded() above we will also need
      // to add a mechanism to wait for already in-progress loads.
      blob_ = getObjectStore()->getBlob(state->hash.value());
    }

    // Write the blob contents out to the overlay
    auto iov = blob_->getContents().getIov();
    folly::writeFileAtomic(
        filePath.stringPiece(), iov.data(), iov.size(), 0600);
    file_ = folly::File(filePath.c_str(), O_RDWR);

    sha1 = getObjectStore()->getSha1ForBlob(state->hash.value());
  }

  // Copy and apply the sha1 to the new file.  This saves us from
  // recomputing it again in the case that something opens the file
  // read/write and closes it without changing it.
  storeSha1(state, sha1);

  // Update the FileInode to indicate that we are materialized now
  blob_.reset();
  state->hash = folly::none;

  return makeFuture();
}

Hash FileData::getSha1() {
  auto state = inode_->state_.wlock();
  if (file_) {
    std::string shastr;
    if (sha1Valid_) {
      shastr = fgetxattr(file_.fd(), kXattrSha1);
    }
    if (shastr.empty()) {
      return recomputeAndStoreSha1(state);
    } else {
      return Hash(shastr);
    }
  }

  CHECK(state->hash.hasValue());
  return getObjectStore()->getSha1ForBlob(state->hash.value());
}

ObjectStore* FileData::getObjectStore() const {
  return inode_->getMount()->getObjectStore();
}

Hash FileData::recomputeAndStoreSha1(
    const folly::Synchronized<FileInode::State>::LockedPtr& state) {
  uint8_t buf[8192];
  off_t off = 0;
  SHA_CTX ctx;
  SHA1_Init(&ctx);

  while (true) {
    // Using pread here so that we don't move the file position;
    // the file descriptor is shared between multiple file handles
    // and while we serialize the requests to FileData, it seems
    // like a good property of this function to avoid changing that
    // state.
    auto len = folly::preadNoInt(file_.fd(), buf, sizeof(buf), off);
    if (len == 0) {
      break;
    }
    if (len == -1) {
      folly::throwSystemError();
    }
    SHA1_Update(&ctx, buf, len);
    off += len;
  }

  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1_Final(digest, &ctx);
  auto sha1 = Hash(folly::ByteRange(digest, sizeof(digest)));
  storeSha1(state, sha1);
  return sha1;
}

void FileData::storeSha1(
    const folly::Synchronized<FileInode::State>::LockedPtr& /* state */,
    Hash sha1) {
  try {
    fsetxattr(file_.fd(), kXattrSha1, sha1.toString());
    sha1Valid_ = true;
  } catch (const std::exception& ex) {
    // If something goes wrong storing the attribute just log a warning
    // and leave sha1Valid_ as false.  We'll have to recompute the value
    // next time we need it.
    LOG(WARNING) << "error setting SHA1 attribute in the overlay: "
                 << folly::exceptionStr(ex);
  }
}
}
}
