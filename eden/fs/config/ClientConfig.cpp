/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "ClientConfig.h"
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/json.h>

using std::string;

namespace {
// INI config file
const facebook::eden::RelativePathPiece kLocalConfig{"edenrc"};

// Keys for the config INI file.
constexpr folly::StringPiece kBindMountsKey{"bindmounts "};
constexpr folly::StringPiece kRepositoryKey{"repository "};
constexpr folly::StringPiece kRepoSection{"repository"};
constexpr folly::StringPiece kName{"name"};
constexpr folly::StringPiece kRepoHooksKey{"hooks"};
constexpr folly::StringPiece kRepoTypeKey{"type"};
constexpr folly::StringPiece kRepoSourceKey{"path"};
constexpr folly::StringPiece kPathsSection{"__paths__"};
constexpr folly::StringPiece kEtcEdenDir{"etc-eden"};
constexpr folly::StringPiece kUserConfigFile{"user-config"};
constexpr folly::StringPiece kConfigDotD{"config.d"};

// Files of interest in the client directory.
const facebook::eden::RelativePathPiece kSnapshotFile{"SNAPSHOT"};
const facebook::eden::RelativePathPiece kBindMountsDir{"bind-mounts"};
const facebook::eden::RelativePathPiece kCloneSuccessFile{"clone-succeeded"};
const facebook::eden::RelativePathPiece kOverlayDir{"local"};
const facebook::eden::RelativePathPiece kDirstateFile{"dirstate"};

// File holding mapping of client directories.
const facebook::eden::RelativePathPiece kClientDirectoryMap{"config.json"};
}

namespace facebook {
namespace eden {

ClientConfig::ClientConfig(
    AbsolutePathPiece mountPath,
    AbsolutePathPiece clientDirectory)
    : clientDirectory_(clientDirectory), mountPath_(mountPath) {}

Hash ClientConfig::getSnapshotID() const {
  // Read the snapshot.
  auto snapshotFile = getSnapshotPath();
  string snapshotFileContents;
  folly::readFile(snapshotFile.c_str(), snapshotFileContents);
  // Make sure to remove any leading or trailing whitespace.
  auto snapshotID = folly::trimWhitespace(snapshotFileContents);
  return Hash{snapshotID};
}

void ClientConfig::setSnapshotID(Hash id) const {
  auto snapshotPath = getSnapshotPath();
  auto hashStr = id.toString() + "\n";
  folly::writeFileAtomic(
      snapshotPath.stringPiece(), folly::StringPiece(hashStr));
}

const AbsolutePath& ClientConfig::getClientDirectory() const {
  return clientDirectory_;
}

AbsolutePath ClientConfig::getSnapshotPath() const {
  return clientDirectory_ + kSnapshotFile;
}

AbsolutePath ClientConfig::getOverlayPath() const {
  return clientDirectory_ + kOverlayDir;
}

AbsolutePath ClientConfig::getCloneSuccessPath() const {
  return clientDirectory_ + kCloneSuccessFile;
}

AbsolutePath ClientConfig::getDirstateStoragePath() const {
  return clientDirectory_ + kDirstateFile;
}

ClientConfig::ConfigData ClientConfig::loadConfigData(
    AbsolutePathPiece etcEdenDirectory,
    AbsolutePathPiece configPath) {
  // Get global config files
  boost::filesystem::path rcDir(
      folly::to<string>(etcEdenDirectory, "/", kConfigDotD));
  std::vector<string> rcFiles;
  if (boost::filesystem::is_directory(rcDir)) {
    for (auto it : boost::filesystem::directory_iterator(rcDir)) {
      rcFiles.push_back(it.path().string());
    }
  }
  sort(rcFiles.begin(), rcFiles.end());

  // Get home config file
  auto userConfigPath = AbsolutePath{configPath};
  rcFiles.push_back(userConfigPath.c_str());

  // A function that prevents merging a repo stanza over a pre-existing one
  auto accept = [](
      const InterpolatedPropertyTree& tree, folly::StringPiece section) {
    if (section.startsWith("repository ") && tree.hasSection(section)) {
      return InterpolatedPropertyTree::MergeDisposition::SkipAll;
    }
    return InterpolatedPropertyTree::MergeDisposition::UpdateAll;
  };

  // Define replacements for use in interpolating the config files.
  // These are coupled with the equivalent code in eden/fs/cli/config.py
  // and must be kept in sync.
  ConfigData resultData{{"HOME", getenv("HOME") ? getenv("HOME") : "/"},
                        {"USER", getenv("USER") ? getenv("USER") : ""}};

  // Record the paths that were used, so that we can use them to
  // create default values later on
  resultData.set(kPathsSection, kEtcEdenDir, etcEdenDirectory.stringPiece());
  resultData.set(kPathsSection, kUserConfigFile, userConfigPath.stringPiece());

  // Parse repository data in order to compile them
  for (auto rc : boost::adaptors::reverse(rcFiles)) {
    if (access(rc.c_str(), R_OK) != 0) {
      continue;
    }
    resultData.updateFromIniFile(AbsolutePathPiece(rc), accept);
  }
  return resultData;
}

std::unique_ptr<ClientConfig> ClientConfig::loadFromClientDirectory(
    AbsolutePathPiece mountPath,
    AbsolutePathPiece clientDirectory,
    const ConfigData* configData) {
  // Extract repository name from the client config file
  ConfigData localConfig;
  localConfig.loadIniFile(clientDirectory + kLocalConfig);
  auto repoName = localConfig.get(kRepoSection, kName, "");

  // Get the data of repository repoName from config files
  auto repoHeader = folly::to<string>(kRepositoryKey, repoName);
  if (!configData->hasSection(repoHeader)) {
    throw std::runtime_error("Could not find repository data for " + repoName);
  }

  // Construct ClientConfig object
  auto config = std::make_unique<ClientConfig>(mountPath, clientDirectory);

  // Extract the bind mounts
  auto bindMountHeader = folly::to<string>(kBindMountsKey, repoName);
  auto bindMountPoints = configData->getSection(bindMountHeader);
  AbsolutePath bindMountsPath = clientDirectory + kBindMountsDir;
  for (auto item : bindMountPoints) {
    auto pathInClientDir = bindMountsPath + RelativePathPiece{item.first};
    auto pathInMountDir = mountPath + RelativePathPiece{item.second};
    config->bindMounts_.emplace_back(pathInClientDir, pathInMountDir);
  }

  // Load repository information
  auto repoData = configData->getSection(repoHeader);
  config->repoType_ = repoData[kRepoTypeKey];
  config->repoSource_ = repoData[kRepoSourceKey];

  auto hooksPath = configData->get(
      repoHeader,
      kRepoHooksKey,
      configData->get(kPathsSection, kEtcEdenDir, "/etc/eden") + "/hooks");
  if (hooksPath != "") {
    config->repoHooks_ = AbsolutePath{hooksPath};
  }

  return config;
}

folly::dynamic ClientConfig::loadClientDirectoryMap(AbsolutePathPiece edenDir) {
  // Extract the JSON and strip any comments.
  std::string jsonContents;
  auto configJsonFile = edenDir + kClientDirectoryMap;
  folly::readFile(configJsonFile.c_str(), jsonContents);
  auto jsonWithoutComments = folly::json::stripComments(jsonContents);
  if (jsonWithoutComments.empty()) {
    return folly::dynamic::object();
  }

  // Parse the comment-free JSON while tolerating trailing commas.
  folly::json::serialization_opts options;
  options.allow_trailing_comma = true;
  return folly::parseJson(jsonWithoutComments, options);
}

AbsolutePathPiece ClientConfig::getRepoHooks() const {
  return repoHooks_.hasValue() ? repoHooks_.value()
                               : AbsolutePathPiece{"/etc/eden/hooks"};
}
}
}
