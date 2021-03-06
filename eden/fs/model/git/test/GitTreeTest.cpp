/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/String.h>
#include <gtest/gtest.h>
#include <string>
#include "eden/fs/model/Hash.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/TreeEntry.h"
#include "eden/fs/model/git/GitTree.h"

using namespace facebook::eden;
using folly::StringPiece;
using folly::IOBuf;
using folly::io::Appender;
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

  auto tree = deserializeGitTree(hash, StringPiece(gitTreeObject));
  EXPECT_EQ(11, tree->getTreeEntries().size());
  EXPECT_EQ(treeHash, Hash::sha1(StringPiece{gitTreeObject}).toString())
      << "SHA-1 of contents should match key";

  // Ordinary, non-executable file.
  auto babelrc = tree->getEntryAt(0);
  EXPECT_EQ(
      Hash("3a8f8eb91101860fd8484154885838bf322964d0"), babelrc.getHash());
  EXPECT_EQ(".babelrc", babelrc.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::BLOB, babelrc.getType());
  EXPECT_EQ(facebook::eden::FileType::REGULAR_FILE, babelrc.getFileType());
  EXPECT_EQ(0b0110, babelrc.getOwnerPermissions());
  EXPECT_EQ(
      babelrc.getName(),
      tree->getEntryAt(PathComponentPiece(".babelrc")).getName());

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
  EXPECT_EQ(
      nuclideStartServer.getName(),
      tree->getEntryAt(PathComponentPiece("nuclide-start-server")).getName());

  // Directory.
  auto lib = tree->getEntryAt(3);
  EXPECT_EQ(Hash("e95798e17f694c227b7a8441cc5c7dae50a187d0"), lib.getHash());
  EXPECT_EQ("lib", lib.getName());
  EXPECT_EQ(facebook::eden::TreeEntryType::TREE, lib.getType());
  EXPECT_EQ(facebook::eden::FileType::DIRECTORY, lib.getFileType());
  EXPECT_EQ(0b0111, lib.getOwnerPermissions());
  EXPECT_EQ(
      lib.getName(), tree->getEntryAt(PathComponentPiece("lib")).getName());

  // lab sorts before lib but is not present in the Tree, so ensure that
  // we don't get an entry back here
  EXPECT_EQ(nullptr, tree->getEntryPtr(PathComponentPiece("lab")));
  EXPECT_THROW(tree->getEntryAt(PathComponentPiece("lab")), std::out_of_range);
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

  auto tree = deserializeGitTree(hash, StringPiece(gitTreeObject));
  EXPECT_EQ(5, tree->getTreeEntries().size());
  EXPECT_EQ(treeHash, Hash::sha1(StringPiece{gitTreeObject}).toString())
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

TEST(GitTree, deserializeEmpty) {
  // Test deserializing the empty tree
  auto data = StringPiece("tree 0\x00", 7);
  auto tree = deserializeGitTree(Hash::sha1(data), data);
  EXPECT_EQ(0, tree->getTreeEntries().size());
}

TEST(GitTree, testBadDeserialize) {
  Hash zero("0000000000000000000000000000000000000000");
  // Partial header
  EXPECT_ANY_THROW(deserializeGitTree(zero, StringPiece("tre")));
  EXPECT_ANY_THROW(deserializeGitTree(zero, StringPiece("tree ")));
  EXPECT_ANY_THROW(deserializeGitTree(zero, StringPiece("tree 123")));

  // Length too long
  IOBuf buf(IOBuf::CREATE, 1024);
  auto a = Appender(&buf, 1024);
  a.push(StringPiece("tree 123"));
  a.write<uint8_t>(0);
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));

  // Truncated after an entry mode
  buf.clear();
  a = Appender(&buf, 1024);
  a.push(StringPiece("tree 6"));
  a.write<uint8_t>(0);
  a.push(StringPiece("100644"));
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));

  // Truncated with no nul byte after the name
  buf.clear();
  a = Appender(&buf, 1024);
  a.push(StringPiece("tree 22"));
  a.write<uint8_t>(0);
  a.push(StringPiece("100644 apm-rest-api.md"));
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));

  // Truncated before entry hash
  buf.clear();
  a = Appender(&buf, 1024);
  a.push(StringPiece("tree 23"));
  a.write<uint8_t>(0);
  a.push(StringPiece("100644 apm-rest-api.md"));
  a.write<uint8_t>(0);
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));

  // Non-octal digit in the mode
  buf.clear();
  a = Appender(&buf, 1024);
  a.push(StringPiece("tree 43"));
  a.write<uint8_t>(0);
  a.push(StringPiece("100694 apm-rest-api.md"));
  a.write<uint8_t>(0);
  a.push(Hash("a3c8e5c25e5523322f0ea490173dbdc1d844aefb").getBytes());
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));

  // Trailing nul byte
  buf.clear();
  a = Appender(&buf, 1024);
  a.push(StringPiece("tree 44"));
  a.write<uint8_t>(0);
  a.push(StringPiece("100644 apm-rest-api.md"));
  a.write<uint8_t>(0);
  a.push(Hash("a3c8e5c25e5523322f0ea490173dbdc1d844aefb").getBytes());
  a.write<uint8_t>(0);
  EXPECT_ANY_THROW(deserializeGitTree(zero, &buf));
}

TEST(GitTree, serializeTree) {
  GitTreeSerializer serializer;
  serializer.addEntry(TreeEntry(
      Hash("c66788d87933862e2111a86304b705dd90bbd427"),
      "README.md",
      FileType::REGULAR_FILE,
      0b110));
  serializer.addEntry(TreeEntry(
      Hash("a3c8e5c25e5523322f0ea490173dbdc1d844aefb"),
      "apm-rest-api.md",
      FileType::REGULAR_FILE,
      0b110));
  serializer.addEntry(TreeEntry(
      Hash("de0b8287939193ed239834991be65b96cbfc4508"),
      "build-instructions",
      FileType::DIRECTORY,
      0b111));
  serializer.addEntry(TreeEntry(
      Hash("4576635ff317960be244b1c4adfe2a6eb2eb024d"),
      "contributing-to-packages.md",
      FileType::REGULAR_FILE,
      0b110));
  serializer.addEntry(TreeEntry(
      Hash("44fcc63439371c8c829df00eec6aedbdc4d0e4cd"),
      "contributing.md",
      FileType::SYMLINK,
      0b111));

  auto buf = serializer.finalize();

  // Make sure the tree hash is what we expect
  auto treeHash = Hash::sha1(&buf);
  EXPECT_EQ(Hash("013b7865a6da317bc8d82c7225eb93615f1b1eca"), treeHash);

  // Make sure we can deserialize it and get back the expected entries.
  auto tree = deserializeGitTree(treeHash, &buf);
  EXPECT_EQ(5, tree->getTreeEntries().size());
  EXPECT_EQ("README.md", tree->getEntryAt(0).getName());
  EXPECT_EQ("apm-rest-api.md", tree->getEntryAt(1).getName());
  EXPECT_EQ("build-instructions", tree->getEntryAt(2).getName());
  EXPECT_EQ("contributing-to-packages.md", tree->getEntryAt(3).getName());
  EXPECT_EQ("contributing.md", tree->getEntryAt(4).getName());
}

// Test using GitTreeSerializer after moving it
TEST(GitTree, moveSerializer) {
  GitTreeSerializer serializer2;

  {
    GitTreeSerializer serializer1;
    serializer1.addEntry(TreeEntry(
        Hash("3b18e512dba79e4c8300dd08aeb37f8e728b8dad"),
        "README.md",
        FileType::REGULAR_FILE,
        0b110));

    serializer2 = std::move(serializer1);
  }

  serializer2.addEntry(TreeEntry(
      Hash("43b71c903ff52b9885bd36f3866324ef60e27b9b"),
      "eden",
      FileType::DIRECTORY,
      0b111));

  // Make sure the tree hash is what we expect
  auto buf = serializer2.finalize();
  auto treeHash = Hash::sha1(&buf);
  EXPECT_EQ(Hash("daa1785514e56d64549d8169ec7dc26803d2f7df"), treeHash);
}

string toBinaryHash(string hex) {
  string bytes;
  folly::unhexlify(hex, bytes);
  return bytes;
}
