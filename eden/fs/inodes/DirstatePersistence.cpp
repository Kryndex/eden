/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "DirstatePersistence.h"

#include <folly/FileUtil.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace facebook {
namespace eden {

using apache::thrift::CompactSerializer;

void DirstatePersistence::save(
    const std::unordered_map<RelativePath, overlay::UserStatusDirective>&
        userDirectives) {
  overlay::DirstateData dirstateData;
  std::map<std::string, overlay::UserStatusDirective> directives;
  for (auto& pair : userDirectives) {
    directives[pair.first.stringPiece().str()] = pair.second;
  }
  dirstateData.directives = directives;
  auto serializedData = CompactSerializer::serialize<std::string>(dirstateData);

  folly::writeFileAtomic(storageFile_.stringPiece(), serializedData, 0644);
}

std::unordered_map<RelativePath, overlay::UserStatusDirective>
DirstatePersistence::load() {
  std::string serializedData;
  std::unordered_map<RelativePath, overlay::UserStatusDirective> entries;
  if (!folly::readFile(storageFile_.c_str(), serializedData)) {
    int err = errno;
    if (err == ENOENT) {
      return entries;
    }
    folly::throwSystemErrorExplicit(err, "failed to read ", storageFile_);
  }

  auto dirstateData =
      CompactSerializer::deserialize<overlay::DirstateData>(serializedData);
  for (auto& pair : dirstateData.directives) {
    auto name = overlay::_UserStatusDirective_VALUES_TO_NAMES.find(pair.second);
    if (name != overlay::_UserStatusDirective_VALUES_TO_NAMES.end()) {
      entries[RelativePath(pair.first)] = pair.second;
    } else {
      throw std::runtime_error(folly::to<std::string>(
          "Illegal enum value for UserStatusDirective: ", pair.second));
    }
  }

  return entries;
}
}
}
