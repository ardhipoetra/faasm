#include "WAVMWasmModule.h"
#include "syscalls.h"

#include <faabric/util/bytes.h>
#include <faabric/util/config.h>
#include <faabric/util/logging.h>
#include <faabric/util/macros.h>
#include <faabric/util/timing.h>

#include <conf/FaasmConfig.h>
#include <storage/FileDescriptor.h>
#include <storage/FileLoader.h>

#include <cstring>
#include <dirent.h>
#include <poll.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <WAVM/Runtime/Intrinsics.h>
#include <WAVM/Runtime/Runtime.h>
#include <WAVM/WASI/WASIABI.h>

/**
 * WASI filesystem handling
 *
 * The main WASI repo contains a fair bit of info on WASI's filesystem handling.
 *
 * https://github.com/WebAssembly/WASI/blob/main/phases/snapshot/docs.md
 *
 * You can also look at the WASI API:
 * https://github.com/WebAssembly/wasi-libc/blob/main/libc-bottom-half/headers/public/wasi/api.h
 */

using namespace WAVM;

namespace wasm {

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_prestat_get",
                               I32,
                               wasi_fd_prestat_get,
                               I32 fd,
                               I32 prestatPtr)
{
    SPDLOG_TRACE("S - fd_prestat_get - {} {}", fd, prestatPtr);

    WAVMWasmModule* module = getExecutingWAVMModule();
    if (!module->getFileSystem().fileDescriptorExists(fd)) {
        return __WASI_EBADF;
    }

    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(fd);

    auto wasiPrestat =
      &Runtime::memoryRef<__wasi_prestat_t>(module->defaultMemory, prestatPtr);
    wasiPrestat->pr_type = fileDesc.wasiPreopenType;
    wasiPrestat->u.dir.pr_name_len = fileDesc.getPath().size();

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_prestat_dir_name",
                               I32,
                               wasi_fd_prestat_dir_name,
                               I32 fd,
                               I32 resPathPtr,
                               I32 resPathLen)
{
    SPDLOG_TRACE("S - fd_prestat_dir_name - {} {}", fd, resPathPtr, resPathLen);

    WAVMWasmModule* module = getExecutingWAVMModule();
    if (!module->getFileSystem().fileDescriptorExists(fd)) {
        return __WASI_EBADF;
    }

    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(fd);

    // Copy the path into the wasm buffer
    char* buffer = Runtime::memoryArrayPtr<char>(
      module->defaultMemory, resPathPtr, resPathLen);
    const std::string& pathStr = fileDesc.getPath();
    std::copy(pathStr.begin(), pathStr.end(), buffer);

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_open",
                               I32,
                               wasi_path_open,
                               I32 rootFd,
                               I32 lookupFlags,
                               I32 path,
                               I32 pathLen,
                               U32 openFlags,
                               U64 rightsBase,
                               U64 rightsInheriting,
                               I32 fdFlags,
                               I32 resFdPtr)
{
    PROF_START(PathOpen)
    const std::string pathStr = getStringFromWasm(path);

    SPDLOG_DEBUG("S - path_open - {} {} {} {} {} {} {} {} {}",
                 rootFd,
                 lookupFlags,
                 pathStr,
                 pathLen,
                 openFlags,
                 rightsBase,
                 rightsInheriting,
                 fdFlags,
                 resFdPtr);

    // Open a new file descriptor
    // Returns a negative wasi errno if fails
    int fdRes = getExecutingWAVMModule()->getFileSystem().openFileDescriptor(
      rootFd,
      pathStr,
      rightsBase,
      rightsInheriting,
      lookupFlags,
      openFlags,
      fdFlags);

    PROF_END(PathOpen)
    if (fdRes < 0) {
        return -1 * fdRes;
    }

    // Write result to memory location
    Runtime::memoryRef<int>(getExecutingWAVMModule()->defaultMemory, resFdPtr) =
      fdRes;
    return __WASI_ESUCCESS;
}

int doWasiDup(I32 fd)
{
    storage::FileSystem& fs = getExecutingWAVMModule()->getFileSystem();
    int newFd = fs.dup(fd);

    return newFd;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "dup", I32, dup, I32 fd)
{
    SPDLOG_DEBUG("S - dup - {}", fd);
    return doWasiDup(fd);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "__wasi_fd_dup",
                               I32,
                               __wasi_fd_dup,
                               I32 fd,
                               I32 resFdPtr)
{
    SPDLOG_DEBUG("S - fd_dup - {}", fd, resFdPtr);

    int newFd = doWasiDup(fd);
    Runtime::memoryRef<I32>(getExecutingWAVMModule()->defaultMemory, resFdPtr) =
      newFd;

    return __WASI_ESUCCESS;
}

/**
 * This works a little like the normal Linux readdir, in that it will be called
 * repeatedly to get the full listing of a directory.
 *
 * The function should fill the read buffer until it's reached the final "page"
 * of results, at which point the returned size will be smaller than the read
 * buffer.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_readdir",
                               I32,
                               wasi_fd_readdir,
                               I32 fd,
                               I32 buf,
                               I32 bufLen,
                               U64 startCookie,
                               I32 resSizePtr)
{
    SPDLOG_TRACE("S - fd_readdir - {} {} {} {} {}",
                 fd,
                 buf,
                 bufLen,
                 startCookie,
                 resSizePtr);

    storage::FileDescriptor& fileDesc =
      getExecutingWAVMModule()->getFileSystem().getFileDescriptor(fd);

    bool isStartCookie = startCookie == __WASI_DIRCOOKIE_START;
    if (fileDesc.iterStarted() && isStartCookie) {
        // Return invalid if we've already started the iterator but also get the
        // start cookie
        return __WASI_EINVAL;
    } else if (!fileDesc.iterStarted() && !isStartCookie) {
        throw std::runtime_error(
          "No directory iterator exists, and this is not the start cookie");
    }

    U8* buffer = Runtime::memoryArrayPtr<U8>(
      getExecutingWAVMModule()->defaultMemory, buf, bufLen);

    size_t bytesCopied = fileDesc.copyDirentsToWasiBuffer(buffer, bufLen);

    // Set the result
    Runtime::memoryRef<U32>(getExecutingWAVMModule()->defaultMemory,
                            resSizePtr) = bytesCopied;

    return __WASI_ESUCCESS;
}

/**
 * This function is tricky to implement as it's iterating through an unknown
 * number of files in the directory. We will be running two loops, one to fill
 * up the given buffer, and a nested loop to iterate through the native
 * directory listing.
 *
 * We try to be conservative but will throw an exception if things aren't right.
 * A bug here can be hard to find.
 *
 * The musl implementation of readdir seems to require returning (-1 * errno) on
 * error, not -1 as the man pages suggest.
 */
I32 s__getdents64(I32 fd, I32 wasmDirentBuf, I32 wasmDirentBufLen)
{
    SPDLOG_DEBUG(
      "S - getdents64 - {} {} {}", fd, wasmDirentBuf, wasmDirentBufLen);

    Runtime::Memory* memoryPtr = getExecutingWAVMModule()->defaultMemory;
    U8* hostWasmDirentBuf =
      &Runtime::memoryRef<U8>(memoryPtr, (Uptr)wasmDirentBuf);

    // Work out how many dirents we can fit (rounding down)
    size_t wasmDirentSize = sizeof(wasm_dirent64);

    // Create a small native buffer (can't overshoot the wasm offset)
    // Note that this can cause an EINVAL error if too small for the result
    int nativeBufLen = 80;

    U32 wasmBytesRead = 0;
    int wasmDirentCount = 0;

    std::vector<std::byte> nativeBuf(nativeBufLen, std::byte(0));
    // Here we will iterate getting native dirents until we've filled up the
    // wasm buffer supplied
    for (wasmBytesRead = 0;
         wasmBytesRead < wasmDirentBufLen - (2 * wasmDirentSize);) {
        // Make the native syscall. This will read in a list of dirent structs
        // to the buffer We need to read at most two native dirents.
        long nativeBytesRead =
          syscall(SYS_getdents64, fd, nativeBuf.data(), nativeBuf.size());

        if (nativeBytesRead < 0) {
            // Error reading native dirents
            int newErrno = errno;
            return -newErrno;
        } else if (nativeBytesRead == 0) {
            // End of directory
            return wasmBytesRead;
        } else {
            // Now we iterate through the dirents we just got back from the host
            unsigned int nativeOffset;
            for (nativeOffset = 0; nativeOffset < nativeBytesRead;) {
                // If we're going to overshoot on the wasm buffer, we have a
                // problem (worth throwing an exception)
                if (wasmBytesRead > (U32)wasmDirentBufLen) {
                    throw std::runtime_error(
                      "Overshot the end of the dirent buffer");
                }

                // Get a pointer to the native dirent
                auto d =
                  reinterpret_cast<::dirent64*>(&nativeBuf.at(nativeOffset));

                // Copy the relevant info into the wasm dirent.
                struct wasm_dirent64 dWasm
                {};
                dWasm.d_ino = (U32)d->d_ino;
                dWasm.d_type = d->d_type;

                // Copy the name into place
                size_t nameLen = strlen(d->d_name);
                std::copy(d->d_name, d->d_name + nameLen, dWasm.d_name);

                // Copy the wasm dirent into place in wasm memory
                auto dWasmBytes = BYTES(&dWasm);
                std::copy(dWasmBytes,
                          dWasmBytes + wasmDirentSize,
                          hostWasmDirentBuf + wasmBytesRead);

                // Move offsets along
                nativeOffset += d->d_reclen;

                wasmBytesRead += wasmDirentSize;
                wasmDirentCount++;
            }
        }
    }

    return wasmBytesRead;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi, "fd_close", I32, wasi_fd_close, I32 fd)
{
    SPDLOG_DEBUG("S - fd_close - {}", fd);

    // TODO - actually closing here can close the preopened fds which messes
    // things up Ignore for now.

    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_write",
                               I32,
                               wasi_fd_write,
                               I32 fd,
                               I32 iovecsPtr,
                               I32 iovecCount,
                               I32 resBytesWrittenPtr)
{
    storage::FileSystem& fileSystem = getExecutingWAVMModule()->getFileSystem();
    std::string path = fileSystem.getPathForFd(fd);

    SPDLOG_TRACE("S - fd_write - {} {} {} {} ({})",
                 fd,
                 iovecsPtr,
                 iovecCount,
                 resBytesWrittenPtr,
                 path);

    storage::FileDescriptor& fileDesc = fileSystem.getFileDescriptor(fd);

    auto nativeIovecs = wasiIovecsToNativeIovecs(iovecsPtr, iovecCount);
    ssize_t bytesWritten = fileDesc.write(nativeIovecs, iovecCount);
    if (bytesWritten < 0) {
        return fileDesc.getWasiErrno();
    }

    // Catpure stdout if necessary, otherwise write as normal
    conf::FaasmConfig& conf = conf::getFaasmConfig();
    bool isStd = fd <= 2;
    if (isStd && conf.captureStdout == "on") {
        getExecutingWAVMModule()->captureStdout(nativeIovecs.data(),
                                                iovecCount);
    }

    Runtime::memoryRef<int32_t>(getExecutingWAVMModule()->defaultMemory,
                                resBytesWrittenPtr) = bytesWritten;

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_read",
                               I32,
                               wasi_fd_read,
                               I32 fd,
                               I32 iovecsPtr,
                               I32 iovecCount,
                               I32 resBytesRead)
{
    PROF_START(FdRead)
    storage::FileSystem& fileSystem = getExecutingWAVMModule()->getFileSystem();
    std::string path = fileSystem.getPathForFd(fd);

    SPDLOG_TRACE(
      "S - fd_read - {} {} {} ({})", fd, iovecsPtr, iovecCount, path);

    storage::FileDescriptor& fileDesc = fileSystem.getFileDescriptor(fd);
    auto nativeIovecs = wasiIovecsToNativeIovecs(iovecsPtr, iovecCount);

    int bytesRead =
      readv(fileDesc.getLinuxFd(), nativeIovecs.data(), iovecCount);
    Runtime::memoryRef<int>(getExecutingWAVMModule()->defaultMemory,
                            resBytesRead) = (int)bytesRead;

    PROF_END(FdRead)

    return __WASI_ESUCCESS;
}

I32 s__mkdir(I32 pathPtr, I32 mode)
{
    const std::string fakePath = getMaskedPathFromWasm(pathPtr);

    SPDLOG_DEBUG("S - mkdir - {} {}", fakePath, mode);

    int res = mkdir(fakePath.c_str(), mode);
    if (res < 0) {
        SPDLOG_ERROR("Failed to mkdir at {} - code {}", fakePath, res);
        throw std::runtime_error("Failed on mkdir");
    }

    return res;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_create_directory",
                               I32,
                               wasi_path_create_directory,
                               I32 fd,
                               I32 path,
                               I32 pathLen)
{
    const std::string& pathStr = getStringFromWasm(path);
    SPDLOG_DEBUG("S - path_create_directory - {} {}", fd, pathStr);

    storage::FileDescriptor& fileDesc =
      getExecutingWAVMModule()->getFileSystem().getFileDescriptor(fd);
    bool success = fileDesc.mkdir(pathStr);

    if (!success) {
        return fileDesc.getWasiErrno();
    }

    return __WASI_ESUCCESS;
}

I32 s__rename(I32 srcPtr, I32 destPtr)
{
    const std::string srcPath = getMaskedPathFromWasm(srcPtr);
    const std::string destPath = getMaskedPathFromWasm(destPtr);

    SPDLOG_DEBUG("S - rename - {} {}", srcPath, destPath);

    int res = rename(srcPath.c_str(), destPath.c_str());
    if (res != 0) {
        if (res == -EPERM) {
            SPDLOG_ERROR(
              "Permission error renaming {} -> {}", srcPath, destPath);
        } else {
            SPDLOG_ERROR(
              "Failed renaming {} -> {} - code {}", srcPath, destPath, res);
        }

        throw std::runtime_error("Failed renaming file");
    }
    return res;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_rename",
                               I32,
                               wasi_path_rename,
                               I32 fd,
                               I32 oldPath,
                               I32 oldPathLen,
                               I32 newFd,
                               I32 newPath,
                               I32 newPathLen)
{
    std::string oldPathStr = getStringFromWasm(oldPath);
    std::string newPathStr = getStringFromWasm(newPath);

    SPDLOG_DEBUG(
      "S - path_rename - {} {} {} {}", fd, oldPathStr, newFd, newPathStr);

    WAVMWasmModule* module = getExecutingWAVMModule();
    storage::FileDescriptor& oldFileDesc =
      module->getFileSystem().getFileDescriptor(fd);
    storage::FileDescriptor& newFileDesc =
      module->getFileSystem().getFileDescriptor(newFd);

    const std::string& fullNewPath = newFileDesc.absPath(newPathStr);
    bool success = oldFileDesc.rename(fullNewPath, oldPathStr);
    if (!success) {
        return oldFileDesc.getWasiErrno();
    }

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_unlink_file",
                               I32,
                               wasi_path_unlink_file,
                               I32 rootFd,
                               I32 pathPtr,
                               I32 pathLen)
{
    SPDLOG_DEBUG("S - path_unlink_file - {} {}", rootFd, pathPtr);

    std::string pathStr = getStringFromWasm(pathPtr);
    storage::FileDescriptor& fileDesc =
      getExecutingWAVMModule()->getFileSystem().getFileDescriptor(rootFd);
    bool success = fileDesc.unlink(pathStr);

    if (!success) {
        return fileDesc.getWasiErrno();
    }

    return 0;
}

I32 s__unlink(I32 pathPtr)
{
    const std::string fakePath = getMaskedPathFromWasm(pathPtr);

    SPDLOG_DEBUG("S - unlink {}", fakePath);

    int res = unlink(fakePath.c_str());
    if (res < 0) {
        SPDLOG_ERROR("Failed to unlink at {} - code {}", fakePath, res);
        throw std::runtime_error("Failed on mkdir");
    }

    return res;
}

I32 s__access(I32 pathPtr, I32 mode)
{
    const std::string path = getMaskedPathFromWasm(pathPtr);
    SPDLOG_DEBUG("S - access - {} {}", path, mode);

    return access(path.c_str(), mode);
}

I32 s__fstat64(I32 fd, I32 statBufPtr)
{
    SPDLOG_DEBUG("S - fstat64 - {} {}", fd, statBufPtr);

    struct ::stat64 nativeStat
    {};
    int result = fstat64(fd, &nativeStat);

    if (result < 0) {
        int newErrno = errno;
        return -newErrno;
    }

    writeNativeStatToWasmStat(&nativeStat, statBufPtr);

    return 0;
}

I32 s__lstat64(I32 pathPtr, I32 statBufPtr)
{
    const std::string fakePath = getMaskedPathFromWasm(pathPtr);
    SPDLOG_DEBUG("S - lstat - {} {}", fakePath, statBufPtr);

    struct ::stat64 nativeStat
    {};
    lstat64(fakePath.c_str(), &nativeStat);
    writeNativeStatToWasmStat(&nativeStat, statBufPtr);

    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_fdstat_get",
                               I32,
                               wasi_fd_fdstat_get,
                               I32 fd,
                               I32 statPtr)
{

    storage::FileSystem& fileSystem = getExecutingWAVMModule()->getFileSystem();
    std::string path = fileSystem.getPathForFd(fd);
    SPDLOG_DEBUG("S - fd_fdstat_get - {} {} ({})", fd, statPtr, path);

    storage::FileDescriptor& fileDesc = fileSystem.getFileDescriptor(fd);
    storage::Stat statResult = fileDesc.stat();

    if (statResult.failed) {
        SPDLOG_TRACE("Failed stat: {}", statResult.wasiErrno);
        return statResult.wasiErrno;
    }

    auto wasiFdStat = &Runtime::memoryRef<__wasi_fdstat_t>(
      getExecutingWAVMModule()->defaultMemory, statPtr);
    wasiFdStat->fs_filetype = statResult.wasiFiletype;
    wasiFdStat->fs_rights_base = fileDesc.getActualRightsBase();
    wasiFdStat->fs_rights_inheriting = fileDesc.getActualRightsInheriting();

    // TODO - set fs flags
    wasiFdStat->fs_flags = 0;

    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_fdstat_set_rights",
                               I32,
                               wasi_fd_fdstat_set_rights,
                               I32 a,
                               I64 b,
                               I64 c)
{
    SPDLOG_DEBUG("S - fd_fdstat_set_rights - {} {} {}", a, b, c);

    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

I32 doFileStat(int fd, const std::string& relativePath, I32 statPtr)
{
    WAVMWasmModule* module = getExecutingWAVMModule();
    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(fd);
    auto wasiFileStat =
      &Runtime::memoryRef<__wasi_filestat_t>(module->defaultMemory, statPtr);

    storage::Stat fileStat = fileDesc.stat(relativePath);
    if (fileStat.failed) {
        return fileStat.wasiErrno;
    }

    wasiFileStat->st_dev = fileStat.st_dev;
    wasiFileStat->st_ino = fileStat.st_ino;
    wasiFileStat->st_filetype = fileStat.wasiFiletype;
    wasiFileStat->st_nlink = fileStat.st_nlink;
    wasiFileStat->st_size = fileStat.st_size;
    wasiFileStat->st_atim = fileStat.st_atim;
    wasiFileStat->st_mtim = fileStat.st_mtim;
    wasiFileStat->st_ctim = fileStat.st_ctim;

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_filestat_get",
                               I32,
                               wasi_fd_filestat_get,
                               I32 fd,
                               I32 statPtr)
{
    SPDLOG_DEBUG("S - fd_filestat_get - {} {}", fd, statPtr);

    return doFileStat(fd, "", statPtr);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_filestat_get",
                               I32,
                               wasi_path_filestat_get,
                               I32 fd,
                               I32 lookupFlags,
                               I32 path,
                               I32 pathLen,
                               I32 statPtr)
{
    const std::string& pathStr = getStringFromWasm(path);
    SPDLOG_TRACE(
      "S - path_filestat_get - {} {} {} {}", fd, lookupFlags, pathStr, statPtr);

    return doFileStat(fd, pathStr, statPtr);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_filestat_set_times",
                               I32,
                               wasi_path_filestat_set_times,
                               I32 fd,
                               I32 lookupFlags,
                               I32 path,
                               I32 pathLen,
                               I64 accessTimeStamp,
                               I64 modTimeStamp,
                               I32 fstFlags)
{
    const std::string& pathStr = getStringFromWasm(path);
    SPDLOG_TRACE("S - path_filestat_set_times - {} {} {} {}",
                 fd,
                 lookupFlags,
                 pathStr,
                 pathLen);

    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_tell",
                               I32,
                               wasi_fd_tell,
                               I32 fd,
                               I32 resOffsetPtr)
{
    SPDLOG_TRACE("S - fd_tell - {} {}", fd, resOffsetPtr);

    WAVMWasmModule* module = getExecutingWAVMModule();

    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(fd);
    uint64_t offset = fileDesc.tell();
    Runtime::memoryRef<U64>(module->defaultMemory, resOffsetPtr) = offset;
    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_seek",
                               I32,
                               wasi_fd_seek,
                               I32 fd,
                               I64 offset,
                               I32 whence,
                               I32 newOffsetPtr)
{
    PROF_START(FdSeek)
    SPDLOG_TRACE("S - fd_seek - {} {} {} {}", fd, offset, whence, newOffsetPtr);

    // Get pointer to result in memory
    auto newOffsetHostPtr = &Runtime::memoryRef<uint64_t>(
      getExecutingWAVMModule()->defaultMemory, newOffsetPtr);

    storage::FileDescriptor& fileDesc =
      getExecutingWAVMModule()->getFileSystem().getFileDescriptor(fd);

    uint16_t wasiErrno = fileDesc.seek(offset, whence, newOffsetHostPtr);

    PROF_END(FdSeek)

    return wasiErrno;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_advise",
                               I32,
                               wasi_fd_advise,
                               I32 fd,
                               I64 offset,
                               I64 len,
                               I32 advice)
{
    SPDLOG_DEBUG("S - fd_advise - {} {} {} {}", fd, offset, len, advice);

    // Ignore fadvise, can't do anything useful with it.

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "ioctl", I32, ioctl, I32 a, I32 b, I32 c)
{
    SPDLOG_DEBUG("S - ioctl - {} {} {}", a, b, c);

    return 0;
}

/**
 * Note here that we assume puts is called on a null-terminated string
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env, "puts", I32, puts, I32 strPtr)
{
    SPDLOG_DEBUG("S - puts - {}", strPtr);
    WAVMWasmModule* module = getExecutingWAVMModule();
    Runtime::Memory* memoryPtr = module->defaultMemory;
    char* hostStr = &Runtime::memoryRef<char>(memoryPtr, (Uptr)strPtr);

    // Capture stdout if necessary
    conf::FaasmConfig& conf = conf::getFaasmConfig();
    if (conf.captureStdout == "on") {
        module->captureStdout(hostStr);
    }

    printf("%s\n", hostStr);
    return 0;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "putc", I32, putc, I32 c, I32 streamPtr)
{
    SPDLOG_DEBUG("S - putc - {} {}", c, streamPtr);

    Runtime::Memory* memoryPtr = getExecutingWAVMModule()->defaultMemory;
    FILE* stream = &Runtime::memoryRef<FILE>(memoryPtr, (Uptr)streamPtr);

    putc(c, stream);

    return 0;
}

/**
 * fprintf can provide some useful debugging info so we can just spit it to
 * stdout
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "vfprintf",
                               I32,
                               vfprintf,
                               I32 fd,
                               U32 formatPtr,
                               I32 argList)
{
    Runtime::Memory* memoryPtr = getExecutingWAVMModule()->defaultMemory;
    const char* format = &Runtime::memoryRef<char>(memoryPtr, (Uptr)formatPtr);
    std::cout << "S - vfprintf - " << format << std::endl;
    return 0;
}

I32 s__readlink(I32 pathPtr, I32 bufPtr, I32 bufLen)
{
    Runtime::Memory* memoryPtr = getExecutingWAVMModule()->defaultMemory;
    char* path = &Runtime::memoryRef<char>(memoryPtr, (Uptr)pathPtr);

    SPDLOG_DEBUG("S - readlink - {} {} {}", path, bufPtr, bufLen);

    char* buf =
      Runtime::memoryArrayPtr<char>(memoryPtr, (Uptr)bufPtr, (Uptr)bufLen);

    ssize_t bytesRead = readlink(path, buf, (size_t)bufLen);

    return (I32)bytesRead;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_readlink",
                               I32,
                               wasi_path_readlink,
                               I32 rootFd,
                               I32 pathPtr,
                               I32 pathLen,
                               I32 buffPtr,
                               I32 buffLen,
                               I32 resBytesUsed)
{
    std::string pathStr = getStringFromWasm(pathPtr);
    SPDLOG_DEBUG("S - path_readlink - {} {} {} {} {}",
                 rootFd,
                 pathStr,
                 buffPtr,
                 buffLen,
                 resBytesUsed);

    WAVMWasmModule* module = getExecutingWAVMModule();
    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(rootFd);

    char* buffer =
      Runtime::memoryArrayPtr<char>(module->defaultMemory, buffPtr, buffLen);
    ssize_t readBytes = fileDesc.readLink(pathStr, buffer, buffLen);
    Runtime::memoryRef<U32>(module->defaultMemory, resBytesUsed) =
      (U32)readBytes;

    return __WASI_ESUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_fdstat_set_flags",
                               I32,
                               wasi_fd_fdstat_set_flags,
                               I32 fd,
                               I32 fdFlags)
{
    SPDLOG_DEBUG("S - fd_fdstat_set_flags - {} {}", fd, fdFlags);

    WAVMWasmModule* module = getExecutingWAVMModule();
    storage::FileDescriptor& fileDesc =
      module->getFileSystem().getFileDescriptor(fd);

    bool success = fileDesc.updateFlags(fdFlags);
    if (!success) {
        return fileDesc.getWasiErrno();
    } else {
        return __WASI_ESUCCESS;
    }
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "bzero", void, bzero, I32 wasmPtr, I32 len)
{
    auto buffer = Runtime::memoryArrayPtr<U8>(
      getExecutingWAVMModule()->defaultMemory, wasmPtr, len);

    ::bzero(buffer, len);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "explicit_bzero",
                               void,
                               explicit_bzero,
                               I32 wasmPtr,
                               I32 len)
{
    auto buffer = Runtime::memoryArrayPtr<U8>(
      getExecutingWAVMModule()->defaultMemory, wasmPtr, len);

    ::explicit_bzero(buffer, len);
}

// -----------------------------
// Unsupported
// -----------------------------

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "__small_sprintf",
                               I32,
                               __small_sprintf,
                               I32 a,
                               I32 b,
                               I32 c)
{
    SPDLOG_DEBUG("S - __small_sprintf - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_renumber",
                               I32,
                               wasi_fd_renumber,
                               I32 fdOld,
                               I32 fdNew)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "tmpfile", I32, tmpfile)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "umask", I32, umask, I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "msync", I32, msync, I32 a, I32 b, I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "tempnam", I32, tempnam, I32 a, I32 b)
{
    SPDLOG_TRACE("S - tempnam - {} {}", a, b);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "memfd_create",
                               I32,
                               memfd_create,
                               I32 a,
                               I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "setgroups", I32, setgroups, I32 a, I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "fchdir", I32, s__fchdir, I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "chmod", I32, s__chmod, I32 a, I32 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_datasync",
                               I32,
                               wasi_fd_datasync,
                               I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_pwrite",
                               I32,
                               wasi_fd_pwrite,
                               I32 a,
                               I32 b,
                               I32 c,
                               I64 d,
                               I32 e)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_pread",
                               I32,
                               wasi_fd_pread,
                               I32 a,
                               I32 b,
                               I32 c,
                               I64 d,
                               I32 e)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_filestat_set_size",
                               I32,
                               wasi_fd_filestat_set_size,
                               I32 a,
                               I64 b)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi, "fd_sync", I32, wasi_fd_sync, I32 a)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_allocate",
                               I32,
                               wasi_fd_allocate,
                               I32 a,
                               I64 b,
                               I64 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "fd_filestat_set_times",
                               I32,
                               fd_filestat_set_times,
                               I32 a,
                               I64 b,
                               I64 c,
                               I32 d)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_link",
                               I32,
                               wasi_path_link,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e,
                               I32 f,
                               I32 g)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_symlink",
                               I32,
                               wasi_path_symlink,
                               I32 a,
                               I32 b,
                               I32 c,
                               I32 d,
                               I32 e)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(wasi,
                               "path_remove_directory",
                               I32,
                               wasi_path_remove_directory,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "lockf", I32, lockf, I32 a, I32 b, I64 c)
{
    SPDLOG_DEBUG("S - lockf - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "strncat",
                               I32,
                               strncat,
                               I32 a,
                               I32 b,
                               I32 c)
{
    SPDLOG_DEBUG("S - strncat - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "realpath", I32, realpath, I32 a, U32 b)
{
    SPDLOG_DEBUG("S - realpath - {} {}", a, b);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "dirfd", I32, dirfd, I32 a)
{
    SPDLOG_DEBUG("S - dirfd - {}", a);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "poll", I32, poll, I32 a, I32 b, I32 c)
{
    SPDLOG_DEBUG("S - poll - {} {} {}", a, b, c);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "sendfile",
                               I32,
                               sendfile,
                               I32 out_fd,
                               I32 in_fd,
                               I32 offset,
                               I32 count)
{
    SPDLOG_DEBUG("S - sendfile - {} {} {} {}", out_fd, in_fd, offset, count);
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

// Emscripten-specific functions
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "fiprintf",
                               I32,
                               wasi_fiprintf,
                               I32 a,
                               I32 b,
                               I32 c)
{
    throwException(Runtime::ExceptionTypes::calledUnimplementedIntrinsic);
}

void ioLink() {}
}
