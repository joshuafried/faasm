#include <catch2/catch.hpp>

#include "faasm_fixtures.h"
#include "utils.h"

#include <conf/FaasmConfig.h>
#include <faabric/util/bytes.h>
#include <faabric/util/files.h>
#include <storage/FileDescriptor.h>
#include <storage/FileLoader.h>
#include <storage/SharedFiles.h>

#include <WAVM/WASI/WASIABI.h>
#include <boost/filesystem.hpp>
#include <string_view>

using namespace storage;

namespace tests {

class FileDescriptorTestFixture : public SharedFilesTestFixture
{
  public:
    FileDescriptorTestFixture() { fs.prepareFilesystem(); }

  protected:
    FileSystem fs;
};

TEST_CASE_METHOD(FileDescriptorTestFixture,
                 "Test fd rights propagation",
                 "[storage]")
{
    FileDescriptor& rootFd = fs.getFileDescriptor(DEFAULT_ROOT_FD);

    uint64_t base = rootFd.getActualRightsBase();
    uint64_t inheriting = rootFd.getActualRightsInheriting();

    // Check the base rights only include those for directories
    REQUIRE(!(bool)(base & __WASI_RIGHT_FD_WRITE));
    REQUIRE((bool)(base & __WASI_RIGHT_PATH_OPEN));

    // Check the inherited rights include file rights
    REQUIRE((bool)(inheriting & __WASI_RIGHT_FD_WRITE));
    REQUIRE((bool)(inheriting & __WASI_RIGHT_PATH_OPEN));

    // Set up some new rights
    uint64_t requestedBase = __WASI_RIGHT_FD_WRITE | __WASI_RIGHT_FD_SEEK;
    uint64_t requestedInheriting = __WASI_RIGHT_FD_READ | __WASI_RIGHT_FD_TELL;
    uint32_t openFlags = __WASI_O_CREAT;

    // Create a new fd, requesting the rights
    int newFd = fs.openFileDescriptor(DEFAULT_ROOT_FD,
                                      "/tmp/foobar.txt",
                                      requestedBase,
                                      requestedInheriting,
                                      0,
                                      openFlags,
                                      0);

    FileDescriptor& newDescriptor = fs.getFileDescriptor(newFd);
    uint64_t baseChild = newDescriptor.getActualRightsBase();
    uint64_t baseInheriting = newDescriptor.getActualRightsInheriting();

    // Check the ones we requested are present, but not those that aren't
    REQUIRE((bool)(baseChild & __WASI_RIGHT_FD_WRITE));
    REQUIRE((bool)(baseChild & __WASI_RIGHT_FD_SEEK));
    REQUIRE(!(bool)(baseChild & __WASI_RIGHT_FD_SYNC));

    REQUIRE((bool)(baseInheriting & __WASI_RIGHT_FD_READ));
    REQUIRE((bool)(baseInheriting & __WASI_RIGHT_FD_TELL));
    REQUIRE(!(bool)(baseInheriting & __WASI_RIGHT_FD_WRITE));
    REQUIRE(!(bool)(baseInheriting & __WASI_RIGHT_FD_SEEK));

    // Open the path where we should be able to create and right
    uint32_t lookupFlags = 0;
    uint32_t fdFlags = __WASI_FDFLAG_APPEND;
    bool success = newDescriptor.pathOpen(lookupFlags, openFlags, fdFlags);
    REQUIRE(success);

    int linuxFlags = newDescriptor.getLinuxFlags();
    REQUIRE((bool)(linuxFlags & O_CREAT));
    REQUIRE((bool)(linuxFlags & O_RDWR));
    REQUIRE((bool)(linuxFlags & O_APPEND));

    REQUIRE(!(bool)(linuxFlags & O_DIRECTORY));
    REQUIRE(!(bool)(linuxFlags & O_RDONLY));
    REQUIRE(!(bool)(linuxFlags & O_RSYNC));
}

TEST_CASE_METHOD(FileDescriptorTestFixture, "Test stat and mkdir", "[storage]")
{
    FileDescriptor& fd = fs.getFileDescriptor(DEFAULT_ROOT_FD);

    std::string dummyDir = "fs_test_dir";

    conf::FaasmConfig& conf = conf::getFaasmConfig();
    std::string realDir = conf.runtimeFilesDir + "/" + dummyDir;
    if (boost::filesystem::exists(realDir)) {
        boost::filesystem::remove_all(realDir);
    }

    // Stat non-existent dir
    Stat dirStat = fd.stat(dummyDir);
    REQUIRE(dirStat.wasiErrno == __WASI_ENOENT);
    REQUIRE(dirStat.failed);

    // Create the directory
    fd.mkdir(dummyDir);
    Stat dirStatB = fd.stat(dummyDir);
    REQUIRE(dirStatB.wasiErrno == 0);
    REQUIRE(!dirStatB.failed);
    REQUIRE(dirStatB.wasiFiletype == __WASI_FILETYPE_DIRECTORY);

    // Delete the directory
    fd.rmdir(dummyDir);
    Stat dirStatC = fd.stat(dummyDir);
    REQUIRE(dirStatC.wasiErrno == __WASI_ENOENT);
    REQUIRE(dirStatC.failed);
}

TEST_CASE_METHOD(FileDescriptorTestFixture,
                 "Test creating, renaming and deleting a file",
                 "[storage]")
{
    FileDescriptor& rootFileDesc = fs.getFileDescriptor(DEFAULT_ROOT_FD);

    std::string dummyDir = "fs_test_dir";
    std::string dummyPath = dummyDir + "/dummy_file.txt";

    // Set up the directory
    conf::FaasmConfig& conf = conf::getFaasmConfig();
    std::string realDir = conf.runtimeFilesDir + "/" + dummyDir;
    if (!boost::filesystem::exists(realDir)) {
        boost::filesystem::create_directories(realDir);
    }

    // Remove the file
    std::string realPath = conf.runtimeFilesDir + "/" + dummyPath;
    boost::filesystem::remove(realPath);

    // Stat the file to begin with
    Stat fileStat = rootFileDesc.stat(dummyPath);
    REQUIRE(fileStat.wasiErrno == __WASI_ENOENT);
    REQUIRE(fileStat.failed);

    // Create the file (ignore perms)
    int fileFd = fs.openFileDescriptor(
      DEFAULT_ROOT_FD, dummyPath, 0, 0, 0, __WASI_O_CREAT, 0);
    REQUIRE(fileFd > 0);

    FileDescriptor& fileFileDesc = fs.getFileDescriptor(fileFd);

    // Check it now exists
    Stat fileStatB = rootFileDesc.stat(dummyPath);
    REQUIRE(fileStatB.wasiErrno == 0);
    REQUIRE(!fileStatB.failed);
    REQUIRE(fileStatB.wasiFiletype == __WASI_FILETYPE_REGULAR_FILE);

    // Rename it
    std::string newPath = dummyDir + "/renamed_file.txt";
    fileFileDesc.rename(newPath);

    // Stat new and old
    Stat fileStatC = rootFileDesc.stat(newPath);
    REQUIRE(fileStatC.wasiErrno == 0);
    REQUIRE(!fileStatC.failed);
    REQUIRE(fileStatB.wasiFiletype == __WASI_FILETYPE_REGULAR_FILE);

    Stat fileStatD = rootFileDesc.stat(dummyPath);
    REQUIRE(fileStatD.wasiErrno == __WASI_ENOENT);
    REQUIRE(fileStatD.failed);

    // Now remove it and check
    rootFileDesc.unlink(newPath);

    Stat fileStatE = rootFileDesc.stat(newPath);
    REQUIRE(fileStatE.wasiErrno == __WASI_ENOENT);
    REQUIRE(fileStatE.failed);

    Stat fileStatF = rootFileDesc.stat(dummyPath);
    REQUIRE(fileStatF.wasiErrno == __WASI_ENOENT);
    REQUIRE(fileStatF.failed);
}

TEST_CASE_METHOD(FileDescriptorTestFixture, "Test seek", "[storage]")
{
    storage::FileLoader& loader = storage::getFileLoader();
    std::string dummyPath;
    std::string realPath;
    std::string contentPath;

    std::vector<uint8_t> contents = { 0, 1, 2, 3, 4, 5, 6 };

    SECTION("Local file")
    {
        dummyPath = "dummy_test_file.txt";
        realPath = conf.runtimeFilesDir + "/" + dummyPath;
        contentPath = realPath;
        faabric::util::writeBytesToFile(realPath, contents);
    }

    SECTION("Shared file")
    {
        dummyPath = "faasm://dummy_test_file.txt";
        contentPath = loader.getSharedFileFile("dummy_test_file.txt");

        // This is the path where the file should end up after being synced
        realPath = SharedFiles::realPathForSharedFile(dummyPath);
    }

    // Set up the file
    boost::filesystem::remove(realPath);
    faabric::util::writeBytesToFile(contentPath, contents);

    // Open file descriptor for the file
    int newFd = fs.openFileDescriptor(
      DEFAULT_ROOT_FD, dummyPath, 0, 0, 0, __WASI_O_CREAT, 0);
    REQUIRE(newFd > 0);
    FileDescriptor& newFileDesc = fs.getFileDescriptor(newFd);

    // Check zero initially
    REQUIRE(newFileDesc.tell() == 0);

    // Set to zero and check tell still at zero
    uint64_t actual = 0;
    newFileDesc.seek(0, __WASI_WHENCE_CUR, &actual);
    REQUIRE(actual == 0);
    REQUIRE(newFileDesc.tell() == 0);

    // Skip forward
    newFileDesc.seek(2, __WASI_WHENCE_CUR, &actual);
    REQUIRE(actual == 2);
    REQUIRE(newFileDesc.tell() == 2);

    // Skip forward again
    newFileDesc.seek(1, __WASI_WHENCE_CUR, &actual);
    REQUIRE(actual == 3);
    REQUIRE(newFileDesc.tell() == 3);

    // Skip back
    newFileDesc.seek(-3, __WASI_WHENCE_CUR, &actual);
    REQUIRE(actual == 0);
    REQUIRE(newFileDesc.tell() == 0);

    // Set absolute
    newFileDesc.seek(4, __WASI_WHENCE_SET, &actual);
    REQUIRE(actual == 4);
    REQUIRE(newFileDesc.tell() == 4);

    boost::filesystem::remove(realPath);
}

TEST_CASE_METHOD(FileDescriptorTestFixture,
                 "Test stat and read shared file",
                 "[storage]")
{
    FileDescriptor& rootFileDesc = fs.getFileDescriptor(DEFAULT_ROOT_FD);

    // Set up the shared file
    storage::FileLoader& loader = storage::getFileLoader();
    std::string relativePath = "test/shared-file-stat.txt";
    std::string fullPath = loader.getSharedFileFile(relativePath);
    boost::filesystem::remove(fullPath);

    std::vector<uint8_t> contents = { 0, 1, 2, 3, 4, 5 };
    faabric::util::writeBytesToFile(fullPath, contents);

    // Stat it as a relative path
    std::string sharedPath = std::string(SHARED_FILE_PREFIX) + relativePath;
    const Stat& statRes = rootFileDesc.stat(sharedPath);
    REQUIRE(!statRes.failed);
    REQUIRE(statRes.wasiErrno == 0);
    REQUIRE(statRes.wasiFiletype == __WASI_FILETYPE_REGULAR_FILE);

    // Open it as a shared file
    int fileFd = fs.openFileDescriptor(
      DEFAULT_ROOT_FD, sharedPath, 0, 0, 0, __WASI_O_CREAT, 0);
    REQUIRE(fileFd > 0);
    FileDescriptor& fileFileDesc = fs.getFileDescriptor(fileFd);

    // Stat again using the absolute path
    const Stat& statResB = fileFileDesc.stat();
    REQUIRE(!statResB.failed);
    REQUIRE(statResB.wasiErrno == 0);
    REQUIRE(statResB.wasiFiletype == __WASI_FILETYPE_REGULAR_FILE);

    // Check path of this new file descriptor
    REQUIRE(fileFileDesc.getPath() == sharedPath);

    const std::string& realPath =
      storage::SharedFiles::realPathForSharedFile(sharedPath);
    const std::vector<uint8_t>& actualContents =
      faabric::util::readFileToBytes(realPath);
    REQUIRE(actualContents == contents);
}

void checkWasiDirentInBuffer(uint8_t* buffer, DirEnt e)
{
    size_t wasiDirentSize = sizeof(__wasi_dirent_t);
    auto wasiDirent = faabric::util::unalignedRead<__wasi_dirent_t>(
      reinterpret_cast<std::uint8_t*>(buffer));

    auto direntPathPtr = reinterpret_cast<const char*>(buffer + wasiDirentSize);
    std::string_view direntPath(direntPathPtr, direntPathPtr + e.path.size());

    REQUIRE(wasiDirent.d_namlen == e.path.size());
    REQUIRE(direntPath == e.path);
}

TEST_CASE_METHOD(FileDescriptorTestFixture,
                 "Test readdir iterator and buffer",
                 "[storage]")
{
    // We need to list a big enough directory here to catch issues with long
    // file listings and the underlying syscalls
    std::string dirPath = "/usr/local/faasm/runtime_root/lib/python3.8";
    std::string wasmPath = "lib/python3.8";

    // Get the full directory listing using stdlib
    DIR* dir = opendir(dirPath.c_str());
    std::vector<std::string> expectedList;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        expectedList.push_back(ent->d_name);
    }
    closedir(dir);

    // Check there are actually some files in the directory
    REQUIRE(expectedList.size() > 50);

    // Open the directory
    int dirFd = fs.openFileDescriptor(
      DEFAULT_ROOT_FD, wasmPath, 0, 0, 0, __WASI_O_DIRECTORY, 0);
    REQUIRE(dirFd > 0);

    storage::FileDescriptor& fileDesc = fs.getFileDescriptor(dirFd);

    SECTION("Iterator")
    {
        REQUIRE(fileDesc.iterStarted() == false);
        REQUIRE(fileDesc.iterFinished() == false);

        // Make sure first few items are the same
        int step = 3;
        for (int i = 0; i < step; i++) {
            storage::DirEnt ent = fileDesc.iterNext();
            REQUIRE(ent.next == i + 1);
            REQUIRE(ent.path == expectedList.at(i));
        }

        REQUIRE(fileDesc.iterStarted() == true);
        REQUIRE(fileDesc.iterFinished() == false);

        // Go back one in the iterator and check we get the relevant entry
        fileDesc.iterBack();
        storage::DirEnt backEnt = fileDesc.iterNext();
        REQUIRE(backEnt.path == expectedList.at(step - 1));

        // Reset, and walk through the whole iterator to check the values match
        fileDesc.iterReset();
        REQUIRE(fileDesc.iterStarted() == false);
        REQUIRE(fileDesc.iterFinished() == false);

        std::vector<std::string> actualList;
        while (!fileDesc.iterFinished()) {
            actualList.push_back(fileDesc.iterNext().path);
        }

        REQUIRE(actualList == expectedList);
        REQUIRE(fileDesc.iterStarted() == true);
        REQUIRE(fileDesc.iterFinished() == true);
    }

    SECTION("WASI dirent buffer")
    {
        // Get the first three entries
        storage::DirEnt entA = fileDesc.iterNext();
        storage::DirEnt entB = fileDesc.iterNext();
        storage::DirEnt entC = fileDesc.iterNext();

        // Work out how long a buffer needs to be
        size_t wasiDirentSize = sizeof(__wasi_dirent_t);
        size_t sizeA = wasiDirentSize + entA.path.size();
        size_t sizeB = wasiDirentSize + entB.path.size();
        size_t sizeC = wasiDirentSize + entC.path.size();

        // Reset the iterator
        fileDesc.iterReset();

        // Make a buffer slightly too small for all of them
        std::vector<uint8_t> buffer(sizeA + sizeB + sizeC - 10);

        // Copy into this buffer
        size_t bytesCopied =
          fileDesc.copyDirentsToWasiBuffer(buffer.data(), buffer.size());
        REQUIRE(bytesCopied == buffer.size());

        // Check contents
        checkWasiDirentInBuffer(buffer.data(), entA);
        checkWasiDirentInBuffer(buffer.data() + sizeA, entB);

        // Run on a second buffer and check the third entry is added first to
        // this one
        std::vector<uint8_t> buffer2(sizeC + 10);
        size_t bytesCopied2 =
          fileDesc.copyDirentsToWasiBuffer(buffer2.data(), buffer2.size());
        REQUIRE(bytesCopied2 == buffer2.size());

        checkWasiDirentInBuffer(buffer2.data(), entC);
    }
}
}
