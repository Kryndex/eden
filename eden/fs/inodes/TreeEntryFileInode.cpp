/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "TreeEntryFileInode.h"

#include "FileData.h"
#include "TreeEntryFileHandle.h"
#include "eden/fs/overlay/Overlay.h"
#include "eden/fs/store/LocalStore.h"
#include "eden/fuse/passthru/PassThruInodes.h"

using folly::Future;
using folly::StringPiece;
using folly::checkUnixError;
using std::string;
using std::vector;

const string kXattrSha1 = "user.sha1";

namespace facebook {
namespace eden {

TreeEntryFileInode::TreeEntryFileInode(
    fuse_ino_t ino,
    std::shared_ptr<TreeInode> parentInode,
    const TreeEntry* entry)
    : fusell::FileInode(ino), parentInode_(parentInode), entry_(entry) {}

folly::Future<fusell::Dispatcher::Attr> TreeEntryFileInode::getattr() {
  fusell::Dispatcher::Attr attr;

  if (!entry_) {
    // stat() the overlay file.
    checkUnixError(stat(getLocalPath().c_str(), &attr.st));
    attr.st.st_ino = ino_;
    return attr;
  }

  attr.st.st_ino = ino_;
  switch (entry_->getFileType()) {
    case FileType::SYMLINK:
      attr.st.st_mode = S_IFLNK;
      break;
    case FileType::REGULAR_FILE:
      attr.st.st_mode = S_IFREG;
      break;
    default:
      folly::throwSystemErrorExplicit(
          EINVAL,
          "TreeEntry has an invalid file type: ",
          entry_->getFileType());
  }

  // Bit 1 is the executable flag.  Flesh out all the permission bits based on
  // the executable bit being set or not.
  if (entry_->getOwnerPermissions() & 1) {
    attr.st.st_mode |= 0755;
  } else {
    attr.st.st_mode |= 0644;
  }

  // We don't know the size unless we fetch the data :-/
  auto blob = parentInode_->getStore()->getBlob(entry_->getHash());
  auto buf = blob->getContents();
  attr.st.st_size = buf.computeChainDataLength();

  return attr;
}

folly::Future<std::string> TreeEntryFileInode::readlink() {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!entry_) {
    struct stat st;
    auto localPath = getLocalPath();

    // Figure out how much space we need to hold the symlink target.
    checkUnixError(lstat(localPath.c_str(), &st));

    // Allocate a string of the appropriate size.
    std::string buf;
    buf.resize(st.st_size, 0 /* filled with zeroes */);

    // Read the link into the string buffer.
    auto res = ::readlink(
        localPath.c_str(), &buf[0], buf.size() + 1 /* for nul terminator */);
    checkUnixError(res);
    CHECK_EQ(st.st_size, res) << "symlink size TOCTOU";

    return buf;
  }

  switch (entry_->getFileType()) {
    case FileType::SYMLINK: {
      auto blob = parentInode_->getStore()->getBlob(entry_->getHash());
      auto buf = blob->getContents();
      return buf.moveToFbString().toStdString();
    }

    default:
      // man 2 readlink says:  EINVAL The named file is not a symbolic link.
      folly::throwSystemErrorExplicit(EINVAL);
  }
}

std::shared_ptr<FileData> TreeEntryFileInode::getOrLoadData() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!data_) {
    data_ = std::make_shared<FileData>(
        mutex_, parentInode_->getStore(), parentInode_->getOverlay(), entry_);
  }

  return data_;
}

void TreeEntryFileInode::fileHandleDidClose() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (data_.unique()) {
    // We're the only remaining user, no need to keep it around
    data_.reset();
  }
}

AbsolutePath TreeEntryFileInode::getLocalPath() const {
  return parentInode_->getOverlay()->getLocalDir() +
      fusell::InodeNameManager::get()->resolvePathToNode(getNodeId());
}

folly::Future<fusell::FileHandle*> TreeEntryFileInode::open(
    const struct fuse_file_info& fi) {
  if (!entry_) {
    auto localPath = getLocalPath();
    auto fd = ::open(localPath.c_str(), fi.flags);
    checkUnixError(fd);
    return new fusell::PassThruFileHandle(fd, ino_);
  }

  switch (entry_->getFileType()) {
    case FileType::REGULAR_FILE: {
      if ((fi.flags & (O_RDWR | O_WRONLY)) != 0) {
        // Don't allow writes.
        folly::throwSystemErrorExplicit(EROFS);
      }

      auto data = getOrLoadData();
      SCOPE_EXIT {
        data.reset();
        fileHandleDidClose();
      };
      data->materialize(
          fi.flags,
          fusell::InodeNameManager::get()->resolvePathToNode(getNodeId()));

      return new TreeEntryFileHandle(
          std::static_pointer_cast<TreeEntryFileInode>(shared_from_this()),
          data,
          fi.flags);
    }
    case FileType::SYMLINK:
      // man 2 open says:  ELOOP ... or O_NOFOLLOW was specified but pathname
      // was a symbolic link.
      // We shouldn't really be able to get here in any case.
      folly::throwSystemErrorExplicit(ELOOP);
    default:
      // We really really should never be able to get here.
      folly::throwSystemErrorExplicit(
          EIO, "impossible filetype ", entry_->getFileType());
  }
}

Future<vector<string>> TreeEntryFileInode::listxattr() {
  // Currently, we only return a non-empty vector for regular files, and we
  // assume that the SHA-1 is present without checking the ObjectStore.
  vector<string> attributes;
  if (!entry_ || entry_->getFileType() != FileType::REGULAR_FILE) {
    return attributes;
  }

  attributes.emplace_back(kXattrSha1);
  return attributes;
}

Future<string> TreeEntryFileInode::getxattr(StringPiece name) {
  // Currently, we only support the xattr for the SHA-1 of a regular file.
  if (!entry_ || name != kXattrSha1 ||
      entry_->getFileType() != FileType::REGULAR_FILE) {
    return "";
  }

  // TODO(mbolin): Make this more fault-tolerant. Currently, there is no logic
  // to account for the case where we don't have the SHA-1 for the blob, the
  // hash doesn't correspond to a blob, etc.
  auto sha1 = parentInode_->getStore()->getSha1ForBlob(entry_->getHash());
  return sha1->toString();
}

const TreeEntry* TreeEntryFileInode::getEntry() const {
  return entry_;
}
}
}