/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/Optional.h>
#include <folly/Range.h>
#include "eden/fs/model/git/GitIgnore.h"
#include "eden/fs/model/git/GlobMatcher.h"

namespace facebook {
namespace eden {

/**
 * A single pattern loaded from a .gitignore file.
 *
 * Each line in a .gitignore file is converted into a separate GitIgnorePattern
 * object.  (Except for empty lines, comments, or otherwise invalid lines,
 * which don't result in any GitIgnorePattern.)
 */
class GitIgnorePattern {
 public:
  /**
   * Parse a line from a gitignore file.
   *
   * Returns a GitIgnorePattern, or folly::none if the line did not contain a
   * pattern (e.g., if it was empty or a comment).
   */
  static folly::Optional<GitIgnorePattern> parseLine(folly::StringPiece line);

  virtual ~GitIgnorePattern();
  GitIgnorePattern(GitIgnorePattern&&) = default;
  GitIgnorePattern& operator=(GitIgnorePattern&&) = default;

  /**
   * Check to see if a pathname matches this pattern.
   *
   * The pathname should be relative to the directory where this pattern was
   * loaded from.  For example, if this pattern was loaded from
   * <repo_root>/foo/bar/.gitignore, when testing the file
   * <repo_root>/foo/bar/abc/xyz.txt, pass in the path as "abc/xyz.txt"
   */
  GitIgnore::MatchResult match(RelativePathPiece path) const {
    return match(path, path.basename());
  }

  /**
   * A version of match() that accepts both the path and the basename.
   *
   * The path parameter should still include the basename (it should not be
   * just the dirname component).
   *
   * While match() could just compute the basename on its own, many patterns
   * require the basename, and code checking the ignore status for a path
   * generally checks the path against many patterns across several GitIgnore
   * files.  It is slightly more efficient for the caller to compute the
   * basename once, rather than re-computing it for each pattern that needs it.
   */
  GitIgnore::MatchResult match(
      RelativePathPiece path,
      PathComponentPiece basename) const;

 private:
  /**
   * Flag values that can be bitwise-ORed to create the flags_ value.
   */
  enum Flags : uint32_t {
    // This pattern started with !, indicating we should explicitly include
    // the anything matching it.
    FLAG_INCLUDE = 0x01,
    // The pattern ended with /, indicating it should only match directories.
    FLAG_MUST_BE_DIR = 0x02,
    // The pattern did not contain /, so it only matches against the last
    // component of any path.
    FLAG_BASENAME_ONLY = 0x04,
  };

  GitIgnorePattern(uint32_t flags, GlobMatcher&& matcher);

  GitIgnorePattern(GitIgnorePattern const&) = delete;
  GitIgnorePattern& operator=(GitIgnorePattern const&) = delete;

  /**
   * A bit set of the Flags defined above.
   */
  uint32_t flags_{0};
  /**
   * The GlobMatcher object for performing matching.
   */
  GlobMatcher matcher_;
};
}
}
