/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/String.h>
#include <git2/oid.h>
#include <gtest/gtest.h>
#include <string>
#include "crypto/lib/cpp/CryptoHelper.h"
#include "eden/fs/model/Hash.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/TreeEntry.h"
#include "eden/fs/model/git/GitTree.h"

using facebook::eden::Hash;
using std::string;

string toBinaryHash(string hex);

TEST(GitTree, testDeserialize) {
  // This is a hash for a tree object in https://github.com/facebook/nuclide.git
  // You can verify its contents with:
  // `git cat-file -p 8e073e366ed82de6465d1209d3f07da7eebabb93`.
  string treeHash("8e073e366ed82de6465d1209d3f07da7eebabb93");
  Hash hash(treeHash);

  auto gitTreeObject = folly::to<string>(
      string("tree 424\x00", 9),

      string("100644 .babelrc\x00", 16),
      toBinaryHash("3a8f8eb91101860fd8484154885838bf322964d0"),

      string("100644 .flowconfig\x00", 19),
      toBinaryHash("3610882f48696cc7ca0835929511c9db70acbec6"),

      string("100644 README.md\x00", 17),
      toBinaryHash("c5f15617ed29cd35964dc197a7960aeaedf2c2d5"),

      string("40000 lib\x00", 10),
      toBinaryHash("e95798e17f694c227b7a8441cc5c7dae50a187d0"),

      string("100755 nuclide-start-server\x00", 28),
      toBinaryHash("006babcf5734d028098961c6f4b6b6719656924b"),

      string("100644 package.json\x00", 20),
      toBinaryHash("582591e0f0d92cb63a85156e39abd43ebf103edc"),

      string("40000 scripts\x00", 14),
      toBinaryHash("e664fd28e60a0da25739fdf732f412ab3e91d1e1"),

      string("100644 services-3.json\x00", 23),
      toBinaryHash("3ead3c6cd723f4867bef4444ba18e6ffbf0f711a"),

      string("100644 services-config.json\x00", 28),
      toBinaryHash("bbc8e67499b7f3e1ea850eeda1253be7da5c9199"),

      string("40000 spec\x00", 11),
      toBinaryHash("3bae53a99d080dd851f78e36eb343320091a3d57"),

      string("100644 xdebug.ini\x00", 18),
      toBinaryHash("9ed5bbccd1b9b0077561d14c0130dc086ab27e04"));

  auto tree = deserializeGitTree(hash, gitTreeObject);
  EXPECT_EQ(11, tree->getTreeEntries().size());
  EXPECT_EQ(treeHash, CryptoHelper::bin2hex(CryptoHelper::sha1(gitTreeObject)))
      << "SHA-1 of contents should match key";

  // Ordinary, non-executable file.
  auto babelrc = tree->getEntryAt(0);
  EXPECT_EQ(
      Hash("3a8f8eb91101860fd8484154885838bf322964d0"), babelrc.getHash());
  EXPECT_EQ(".babelrc", babelrc.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::BLOB, babelrc.getType());
  EXPECT_EQ(facebook::eden::FileType::REGULAR_FILE, babelrc.getFileType());
  EXPECT_EQ(0b0110, babelrc.getOwnerPermissions());

  // Executable file.
  auto nuclideStartServer = tree->getEntryAt(4);
  EXPECT_EQ(
      Hash("006babcf5734d028098961c6f4b6b6719656924b"),
      nuclideStartServer.getHash());
  EXPECT_EQ("nuclide-start-server", nuclideStartServer.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::BLOB, nuclideStartServer.getType());
  EXPECT_EQ(
      facebook::eden::FileType::REGULAR_FILE, nuclideStartServer.getFileType());
  EXPECT_EQ(0b0111, nuclideStartServer.getOwnerPermissions());

  // Directory.
  auto lib = tree->getEntryAt(3);
  EXPECT_EQ(Hash("e95798e17f694c227b7a8441cc5c7dae50a187d0"), lib.getHash());
  EXPECT_EQ("lib", lib.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::TREE, lib.getType());
  EXPECT_EQ(facebook::eden::FileType::DIRECTORY, lib.getFileType());
  EXPECT_EQ(0b0111, lib.getOwnerPermissions());
}

TEST(GitTree, testDeserializeWithSymlink) {
  // This is a hash for a tree object in https://github.com/atom/atom.git
  // You can verify its contents with:
  // `git cat-file -p 013b7865a6da317bc8d82c7225eb93615f1b1eca`.
  string treeHash("013b7865a6da317bc8d82c7225eb93615f1b1eca");
  Hash hash(treeHash);

  auto gitTreeObject = folly::to<string>(
      string("tree 223\x00", 9),

      string("100644 README.md\x00", 17),
      toBinaryHash("c66788d87933862e2111a86304b705dd90bbd427"),

      string("100644 apm-rest-api.md\x00", 23),
      toBinaryHash("a3c8e5c25e5523322f0ea490173dbdc1d844aefb"),

      string("40000 build-instructions\x00", 25),
      toBinaryHash("de0b8287939193ed239834991be65b96cbfc4508"),

      string("100644 contributing-to-packages.md\x00", 35),
      toBinaryHash("4576635ff317960be244b1c4adfe2a6eb2eb024d"),

      string("120000 contributing.md\x00", 23),
      toBinaryHash("44fcc63439371c8c829df00eec6aedbdc4d0e4cd"));

  auto tree = deserializeGitTree(hash, gitTreeObject);
  EXPECT_EQ(5, tree->getTreeEntries().size());
  EXPECT_EQ(treeHash, CryptoHelper::bin2hex(CryptoHelper::sha1(gitTreeObject)))
      << "SHA-1 of contents should match key";

  // Ordinary, non-executable file.
  auto contributing = tree->getEntryAt(4);
  EXPECT_EQ(
      Hash("44fcc63439371c8c829df00eec6aedbdc4d0e4cd"), contributing.getHash());
  EXPECT_EQ("contributing.md", contributing.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::BLOB, contributing.getType());
  EXPECT_EQ(facebook::eden::FileType::SYMLINK, contributing.getFileType());
  EXPECT_EQ(0b0111, contributing.getOwnerPermissions());
}

string toBinaryHash(string hex) {
  string bytes;
  folly::unhexlify(hex, bytes);
  return bytes;
}