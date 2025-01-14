/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// We need to prevent winnt.h from defining the core STATUS codes,
// otherwise they will conflict with what we're getting from ntstatus.h
#define UMDF_USING_NTSTATUS

#include <folly/portability/Unistd.h>

#if defined(__APPLE__)
off64_t lseek64(int fh, off64_t off, int orig) {
  return lseek(fh, off, orig);
}

ssize_t pread64(int fd, void* buf, size_t count, off64_t offset) {
  return pread(fd, buf, count, offset);
}

static_assert(
    sizeof(off_t) >= 8, "We expect that Mac OS have at least a 64-bit off_t.");
#endif

#ifdef _WIN32

#include <cstdio>

#include <fcntl.h>

#include <folly/net/detail/SocketFileDescriptorMap.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Windows.h>

template <bool is64Bit, class Offset>
static Offset seek(int fd, Offset offset, int whence) {
  Offset res;
  if (is64Bit) {
    res = lseek64(fd, offset, whence);
  } else {
    res = lseek(fd, offset, whence);
  }
  return res;
}

// Generic wrapper for the p* family of functions.
template <bool is64Bit, class F, class Offset, class... Args>
static int wrapPositional(F f, int fd, Offset offset, Args... args) {
  Offset origLoc = seek<is64Bit>(fd, offset, SEEK_CUR);
  if (origLoc == Offset(-1)) {
    return -1;
  }

  Offset moved = seek<is64Bit>(fd, offset, SEEK_SET);
  if (moved == Offset(-1)) {
    return -1;
  }

  int res = (int)f(fd, args...);

  int curErrNo = errno;
  Offset afterOperation = seek<is64Bit>(fd, origLoc, SEEK_SET);
  if (afterOperation == Offset(-1)) {
    if (res == -1) {
      errno = curErrNo;
    }
    return -1;
  }
  errno = curErrNo;

  return res;
}

namespace folly {
namespace portability {
namespace unistd {
int access(char const* fn, int am) {
  return _access(fn, am);
}

int chdir(const char* path) {
  return _chdir(path);
}

int close(int fh) {
  if (folly::portability::sockets::is_fh_socket(fh)) {
    return netops::detail::SocketFileDescriptorMap::close(fh);
  }
  return _close(fh);
}

int dup(int fh) {
  return _dup(fh);
}

int dup2(int fhs, int fhd) {
  return _dup2(fhs, fhd);
}

int fsync(int fd) {
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }
  if (!FlushFileBuffers(h)) {
    return -1;
  }
  return 0;
}

int ftruncate(int fd, off_t len) {
  off_t origLoc = _lseek(fd, 0, SEEK_CUR);
  if (origLoc == -1) {
    return -1;
  }
  if (_lseek(fd, len, SEEK_SET) == -1) {
    return -1;
  }

  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }
  if (!SetEndOfFile(h)) {
    return -1;
  }
  if (_lseek(fd, origLoc, SEEK_SET) == -1) {
    return -1;
  }
  return 0;
}

char* getcwd(char* buf, int sz) {
  return _getcwd(buf, sz);
}

int getdtablesize() {
  return _getmaxstdio();
}

gid_t getgid() {
  return 1;
}

// No major need to implement this, and getting a non-potentially
// stale ID on windows is a bit involved.
pid_t getppid() {
  return (pid_t)1;
}

uid_t getuid() {
  return 1;
}

int isatty(int fh) {
  return _isatty(fh);
}

int lockf(int fd, int cmd, off_t len) {
  return _locking(fd, cmd, len);
}

off_t lseek(int fh, off_t off, int orig) {
  return _lseek(fh, off, orig);
}

off64_t lseek64(int fh, off64_t off, int orig) {
  return _lseeki64(fh, static_cast<int64_t>(off), orig);
}

int rmdir(const char* path) {
  return _rmdir(path);
}

int pipe(int pth[2]) {
  // We need to be able to listen to pipes with
  // libevent, so they need to be actual sockets.
  return socketpair(PF_UNIX, SOCK_STREAM, 0, pth);
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  const bool is64Bit = false;
  return wrapPositional<is64Bit>(_read, fd, offset, buf, (unsigned int)count);
}

ssize_t pread64(int fd, void* buf, size_t count, off64_t offset) {
  const bool is64Bit = true;
  return wrapPositional<is64Bit>(_read, fd, offset, buf, (unsigned int)count);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  const bool is64Bit = false;
  return wrapPositional<is64Bit>(_write, fd, offset, buf, (unsigned int)count);
}

ssize_t read(int fh, void* buf, size_t count) {
  if (folly::portability::sockets::is_fh_socket(fh)) {
    SOCKET s = (SOCKET)_get_osfhandle(fh);
    if (s != INVALID_SOCKET) {
      auto r = folly::portability::sockets::recv(fh, buf, count, 0);
      if (r == -1 && WSAGetLastError() == WSAEWOULDBLOCK) {
        errno = EAGAIN;
      }
      return r;
    }
  }
  auto r = _read(fh, buf, static_cast<unsigned int>(count));
  if (r == -1 && GetLastError() == ERROR_NO_DATA) {
    // This only happens if the file was non-blocking and
    // no data was present. We have to translate the error
    // to a form that the rest of the world is expecting.
    errno = EAGAIN;
  }
  return r;
}

ssize_t readlink(const char* path, char* buf, size_t buflen) {
  if (!buflen) {
    return -1;
  }

  HANDLE h = CreateFileA(
      path,
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }

  DWORD ret =
      GetFinalPathNameByHandleA(h, buf, DWORD(buflen - 1), VOLUME_NAME_DOS);
  if (ret >= buflen || ret >= MAX_PATH || !ret) {
    CloseHandle(h);
    return -1;
  }

  CloseHandle(h);
  buf[ret] = '\0';
  return ret;
}

void* sbrk(intptr_t /* i */) {
  return (void*)-1;
}

unsigned int sleep(unsigned int seconds) {
  Sleep((DWORD)(seconds * 1000));
  return 0;
}

long sysconf(int tp) {
  switch (tp) {
    case _SC_PAGESIZE: {
      SYSTEM_INFO inf;
      GetSystemInfo(&inf);
      return (long)inf.dwPageSize;
    }
    case _SC_NPROCESSORS_ONLN: {
      SYSTEM_INFO inf;
      GetSystemInfo(&inf);
      return (long)inf.dwNumberOfProcessors;
    }
    default:
      return -1L;
  }
}

int truncate(const char* path, off_t len) {
  int fd = _open(path, O_WRONLY);
  if (!fd) {
    return -1;
  }
  if (ftruncate(fd, len)) {
    _close(fd);
    return -1;
  }
  return _close(fd) ? -1 : 0;
}

int usleep(unsigned int ms) {
  Sleep((DWORD)(ms / 1000));
  return 0;
}

ssize_t write(int fh, void const* buf, size_t count) {
  if (folly::portability::sockets::is_fh_socket(fh)) {
    SOCKET s = (SOCKET)_get_osfhandle(fh);
    if (s != INVALID_SOCKET) {
      auto r = folly::portability::sockets::send(fh, buf, (size_t)count, 0);
      if (r == -1 && WSAGetLastError() == WSAEWOULDBLOCK) {
        errno = EAGAIN;
      }
      return r;
    }
  }
  auto r = _write(fh, buf, static_cast<unsigned int>(count));
  if ((r > 0 && size_t(r) != count) || (r == -1 && errno == ENOSPC)) {
    // Writing to a pipe with a full buffer doesn't generate
    // any error type, unless it caused us to write exactly 0
    // bytes, so we have to see if we have a pipe first. We
    // don't touch the errno for anything else.
    HANDLE h = (HANDLE)_get_osfhandle(fh);
    if (GetFileType(h) == FILE_TYPE_PIPE) {
      DWORD state = 0;
      if (GetNamedPipeHandleState(
              h, &state, nullptr, nullptr, nullptr, nullptr, 0)) {
        if ((state & PIPE_NOWAIT) == PIPE_NOWAIT) {
          errno = EAGAIN;
          return -1;
        }
      }
    }
  }
  return r;
}
} // namespace unistd
} // namespace portability
} // namespace folly

#endif
