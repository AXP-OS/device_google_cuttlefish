workspace(name = "cuttlefish")

#### Google Flags

new_git_repository(
    name = "gflags_repo",
    remote = "https://github.com/gflags/gflags.git",
    tag = "v2.1.2",
    build_file = "//external:gflags.BUILD",
)

bind(
    name = "gflags",
    actual = "@gflags_repo//:gflags",
)

#### Google Logging

new_git_repository(
    name = "glog_repo",
    remote = "https://github.com/google/glog.git",
    tag = "v0.3.4",
    build_file = "//external:glog.BUILD",
)

bind(
    name = "glog",
    actual = "@glog_repo//:glog",
)

#### Google Test

new_http_archive(
    name = "gtest_repo",
    url = "https://github.com/google/googletest/archive/release-1.8.0.zip",
    sha256 = "f3ed3b58511efd272eb074a3a6d6fb79d7c2e6a0e374323d1e6bcbcc1ef141bf",
    strip_prefix = "googletest-release-1.8.0",
    build_file = "//external:gtest.BUILD",
)

bind(
    name = "gtest",
    actual = "@gtest_repo//:gtest",
)

bind(
    name = "gtest_main",
    actual = "@gtest_repo//:gtest_gmock",
)
