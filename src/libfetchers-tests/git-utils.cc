#include "git-utils.hh"
#include "file-system.hh"
#include "gmock/gmock.h"
#include <git2/global.h>
#include <git2/repository.h>
#include <git2/types.h>
#include <gtest/gtest.h>
#include "fs-sink.hh"
#include "serialise.hh"
#include "git-lfs-fetch.hh"

namespace nix {

class GitUtilsTest : public ::testing::Test
{
    // We use a single repository for all tests.
    Path tmpDir;
    std::unique_ptr<AutoDelete> delTmpDir;

public:
    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);

        // Create the repo with libgit2
        git_libgit2_init();
        git_repository * repo = nullptr;
        auto r = git_repository_init(&repo, tmpDir.c_str(), 0);
        ASSERT_EQ(r, 0);
        git_repository_free(repo);
    }

    void TearDown() override
    {
        // Destroy the AutoDelete, triggering removal
        // not AutoDelete::reset(), which would cancel the deletion.
        delTmpDir.reset();
    }

    ref<GitRepo> openRepo()
    {
        return GitRepo::openRepo(tmpDir, true, false);
    }
};

void writeString(CreateRegularFileSink & fileSink, std::string contents, bool executable)
{
    if (executable)
        fileSink.isExecutable();
    fileSink.preallocateContents(contents.size());
    fileSink(contents);
}

TEST_F(GitUtilsTest, sink_basic)
{
    auto repo = openRepo();
    auto sink = repo->getFileSystemObjectSink();

    // TODO/Question: It seems a little odd that we use the tarball-like convention of requiring a top-level directory
    // here
    //                The sync method does not document this behavior, should probably renamed because it's not very
    //                general, and I can't imagine that "non-conventional" archives or any other source to be handled by
    //                this sink.

    sink->createDirectory(CanonPath("foo-1.1"));

    sink->createRegularFile(CanonPath("foo-1.1/hello"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "hello world", false);
    });
    sink->createRegularFile(CanonPath("foo-1.1/bye"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "thanks for all the fish", false);
    });
    sink->createSymlink(CanonPath("foo-1.1/bye-link"), "bye");
    sink->createDirectory(CanonPath("foo-1.1/empty"));
    sink->createDirectory(CanonPath("foo-1.1/links"));
    sink->createHardlink(CanonPath("foo-1.1/links/foo"), CanonPath("foo-1.1/hello"));

    // sink->createHardlink("foo-1.1/links/foo-2", CanonPath("foo-1.1/hello"));

    auto result = repo->dereferenceSingletonDirectory(sink->flush());
    auto accessor = repo->getAccessor(result, false);
    auto entries = accessor->readDirectory(CanonPath::root);
    ASSERT_EQ(entries.size(), 5);
    ASSERT_EQ(accessor->readFile(CanonPath("hello")), "hello world");
    ASSERT_EQ(accessor->readFile(CanonPath("bye")), "thanks for all the fish");
    ASSERT_EQ(accessor->readLink(CanonPath("bye-link")), "bye");
    ASSERT_EQ(accessor->readDirectory(CanonPath("empty")).size(), 0);
    ASSERT_EQ(accessor->readFile(CanonPath("links/foo")), "hello world");
};

TEST_F(GitUtilsTest, sink_hardlink)
{
    auto repo = openRepo();
    auto sink = repo->getFileSystemObjectSink();

    sink->createDirectory(CanonPath("foo-1.1"));

    sink->createRegularFile(CanonPath("foo-1.1/hello"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "hello world", false);
    });

    try {
        sink->createHardlink(CanonPath("foo-1.1/link"), CanonPath("hello"));
        FAIL() << "Expected an exception";
    } catch (const nix::Error & e) {
        ASSERT_THAT(e.msg(), testing::HasSubstr("cannot find hard link target"));
        ASSERT_THAT(e.msg(), testing::HasSubstr("/hello"));
        ASSERT_THAT(e.msg(), testing::HasSubstr("foo-1.1/link"));
    }
};

namespace lfs {

TEST_F(GitUtilsTest, parseGitRemoteUrl)
{
    {
        GitUrl result = parseGitUrl("git@example.com:path/repo.git");
        EXPECT_EQ(result.protocol, "ssh");
        EXPECT_EQ(result.user, "git");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("example.com:/path/repo.git");
        EXPECT_EQ(result.protocol, "ssh");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "/path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("example.com:path/repo.git");
        EXPECT_EQ(result.protocol, "ssh");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("https://example.com/path/repo.git");
        EXPECT_EQ(result.protocol, "https");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("ssh://git@example.com/path/repo.git");
        EXPECT_EQ(result.protocol, "ssh");
        EXPECT_EQ(result.user, "git");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("ssh://example/path/repo.git");
        EXPECT_EQ(result.protocol, "ssh");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "example");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("http://example.com:8080/path/repo.git");
        EXPECT_EQ(result.protocol, "http");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "example.com");
        EXPECT_EQ(result.port, "8080");
        EXPECT_EQ(result.path, "path/repo.git");
    }

    {
        GitUrl result = parseGitUrl("invalid-url");
        EXPECT_EQ(result.protocol, "");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "");
    }

    {
        GitUrl result = parseGitUrl("");
        EXPECT_EQ(result.protocol, "");
        EXPECT_EQ(result.user, "");
        EXPECT_EQ(result.host, "");
        EXPECT_EQ(result.port, "");
        EXPECT_EQ(result.path, "");
    }
}
TEST_F(GitUtilsTest, gitUrlToHttp)
{
    {
        const GitUrl url = parseGitUrl("git@github.com:user/repo.git");
        EXPECT_EQ(url.toHttp(), "https://github.com/user/repo.git");
    }
    {
        const GitUrl url = parseGitUrl("https://github.com/user/repo.git");
        EXPECT_EQ(url.toHttp(), "https://github.com/user/repo.git");
    }
    {
        const GitUrl url = parseGitUrl("http://github.com/user/repo.git");
        EXPECT_EQ(url.toHttp(), "http://github.com/user/repo.git");
    }
    {
        const GitUrl url = parseGitUrl("ssh://git@github.com:22/user/repo.git");
        EXPECT_EQ(url.toHttp(), "https://github.com:22/user/repo.git");
    }
    {
        const GitUrl url = parseGitUrl("invalid-url");
        EXPECT_EQ(url.toHttp(), "");
    }
}

TEST_F(GitUtilsTest, gitUrlToSsh)
{
    {
        const GitUrl url = parseGitUrl("https://example.com/user/repo.git");
        const auto [host, path] = url.toSsh();
        EXPECT_EQ(host, "example.com");
        EXPECT_EQ(path, "user/repo.git");
    }
    {
        const GitUrl url = parseGitUrl("git@example.com:user/repo.git");
        const auto [host, path] = url.toSsh();
        EXPECT_EQ(host, "git@example.com");
        EXPECT_EQ(path, "user/repo.git");
    }
}

} // namespace lfs

} // namespace nix
