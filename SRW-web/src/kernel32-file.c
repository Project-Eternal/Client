/* kernel32: files, directories, paths, file mappings and the environment, on top of the virtual file system */
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_remote.h"
#include "srw_win32.h"

#define INVALID_HANDLE_VALUE 0xffffffffu
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define INVALID_SET_FILE_POINTER 0xffffffffu

enum {
    FILE_ATTRIBUTE_READONLY = 0x1,
    FILE_ATTRIBUTE_DIRECTORY = 0x10,
    FILE_ATTRIBUTE_NORMAL = 0x80
};

enum {
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_PATH_NOT_FOUND = 3,
    ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6,
    ERROR_NO_MORE_FILES = 18,
    ERROR_FILE_EXISTS = 80,
    ERROR_INVALID_PARAMETER = 87,
    ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_ALREADY_EXISTS = 183
};

typedef struct {
    uint32_t low;
    uint32_t high;
} FileTime;

#pragma pack(push, 1)
typedef struct {
    uint32_t dwFileAttributes;
    FileTime ftCreationTime;
    FileTime ftLastAccessTime;
    FileTime ftLastWriteTime;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t dwReserved0;
    uint32_t dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} FindData;

typedef struct {
    uint32_t dwFileAttributes;
    FileTime ftCreationTime;
    FileTime ftLastAccessTime;
    FileTime ftLastWriteTime;
    uint32_t dwVolumeSerialNumber;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t nNumberOfLinks;
    uint32_t nFileIndexHigh;
    uint32_t nFileIndexLow;
} FileInformation;
#pragma pack(pop)

static uint32_t error_from_errno(int error)
{
    switch (error)
    {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR:
    case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

static void make_file_time(time_t seconds, FileTime *out)
{
    uint64_t ticks = ((uint64_t) seconds + 11644473600ull) * 10000000ull;

    out->low = (uint32_t) ticks;
    out->high = (uint32_t) (ticks >> 32);
}

static uint32_t attributes_of(const struct stat *st)
{
    uint32_t attributes = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    if ((st->st_mode & 0222) == 0) attributes |= FILE_ATTRIBUTE_READONLY;
    return attributes;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* file objects */

typedef struct {
    SRW_Object object;
    int fd;                  /* -1 for a file that is streamed */
    SRW_RemoteFile *remote;
    uint64_t position;       /* of a streamed file */
    int echo;                /* a log of the program (Logs/*.log): what it writes is repeated in the log of the runtime (diagnostics) */
} FileObject;

/* what the program does with the streamed files (first operations only) */
static void trace_remote(const char *operation, const FileObject *object, uint64_t a, uint64_t b)
{
    static int logged;
    const char *path;

    if (object == NULL || object->remote == NULL || logged >= 80) return;
    path = SRW_RemotePath(object->remote);
    logged++;
    SRW_LOG(SRW_LOG_FILES, "%s %s %llu %llu", operation, path, (unsigned long long) a, (unsigned long long) b);
}

static void destroy_file(SRW_Object *object)
{
    trace_remote("close", (FileObject *) object, 0, 0);
    if (((FileObject *) object)->fd >= 0) close(((FileObject *) object)->fd);
}

/* the file object of a handle with a reference (file_unref when done: another thread may close the handle meanwhile) */
static FileObject *file_ref(uint32_t handle)
{
    SRW_Object *object = SRW_ObjectRef(handle, SRW_OBJ_FILE);

    if (object == NULL)
    {
        logprintf("File handle %x is not valid", (unsigned int) handle);
        SRW_SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return (FileObject *) object;
}

static void file_unref(FileObject *file)
{
    SRW_ObjectUnref((SRW_Object *) file);
}

/* the descriptor of a file object (not a streamed one), or -1 */
static int fd_of(FileObject *file)
{
    if (file == NULL) return -1;
    if (file->fd < 0)
    {
        trace_remote("refused operation on", file, 0, 0);
        SRW_SetLastError(ERROR_ACCESS_DENIED); /* an operation a streamed file does not have */
        return -1;
    }
    return file->fd;
}

/* whether a handle is a file with a descriptor */
static int file_has_fd(uint32_t handle)
{
    FileObject *file = file_ref(handle);
    int ok = fd_of(file) >= 0;

    file_unref(file);
    return ok;
}

static uint32_t open_remote(SRW_RemoteFile *remote)
{
    FileObject *file = calloc(1, sizeof(*file));
    uint32_t handle;

    if (file == NULL) return INVALID_HANDLE_VALUE;
    file->object.type = SRW_OBJ_FILE;
    file->object.destroy = destroy_file;
    file->fd = -1;
    file->remote = remote;
    handle = SRW_ObjectCreate(&file->object);
    if (handle == 0)
    {
        free(file);
        SRW_SetLastError(4);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

/* the attributes of an open file, also for a streamed one */
static int stat_handle(uint32_t handle, struct stat *st)
{
    FileObject *object = file_ref(handle);
    int result;

    if (object == NULL) return -1;
    if (object->remote != NULL)
    {
        trace_remote("information about", object, 0, 0);
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0444;
        st->st_size = (off_t) SRW_RemoteSize(object->remote);
        st->st_mtime = st->st_ctime = st->st_atime = 1130000000; /* the program's own date is not known */
        result = 0;
    }
    else result = fstat(object->fd, st);
    file_unref(object);
    return result;
}

uint32_t CCALL CreateFileA_c(const char *lpFileName, uint32_t dwDesiredAccess, uint32_t dwShareMode, void *lpSecurityAttributes,
                             uint32_t dwCreationDisposition, uint32_t dwFlagsAndAttributes, uint32_t hTemplateFile)
{
    char path[520];
    int flags, fd;
    FileObject *file;
    uint32_t handle;

    (void) dwShareMode;
    (void) lpSecurityAttributes;
    (void) dwFlagsAndAttributes;
    (void) hTemplateFile;

    SRW_MapPath(lpFileName, path, sizeof(path));
    SRW_LOG(SRW_LOG_FILES, "CreateFile(\"%s\" -> \"%s\", access %x, disposition %u)", lpFileName, path, (unsigned int) dwDesiredAccess, (unsigned int) dwCreationDisposition);

    if ((dwDesiredAccess & 0x40000000u) == 0 && (dwCreationDisposition == 3 || dwCreationDisposition == 4))
    {
        SRW_RemoteFile *remote = SRW_RemoteFind(path);

        if (remote != NULL) return open_remote(remote);
    }

    if ((dwDesiredAccess & 0x40000000u) != 0) /* GENERIC_WRITE */
    {
        flags = ((dwDesiredAccess & 0x80000000u) != 0) ? O_RDWR : O_WRONLY;
    }
    else
    {
        flags = O_RDONLY;
    }
    switch (dwCreationDisposition)
    {
    case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    default:
        SRW_SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    fd = open(path, flags, 0666);
    if (fd < 0)
    {
        SRW_LOG(SRW_LOG_FILES, "CreateFile(\"%s\") failed: %s", path, strerror(errno));
        SRW_SetLastError((errno == EEXIST) ? ERROR_FILE_EXISTS : error_from_errno(errno));
        return INVALID_HANDLE_VALUE;
    }

    file = calloc(1, sizeof(*file));
    if (file == NULL)
    {
        close(fd);
        SRW_SetLastError(8);
        return INVALID_HANDLE_VALUE;
    }
    file->object.type = SRW_OBJ_FILE;
    file->object.destroy = destroy_file;
    file->fd = fd;
#ifdef SRW_DIAGNOSTICS
    if ((dwDesiredAccess & 0x40000000u) != 0 && strstr(path, "/Logs/") != NULL && strlen(path) > 4 && strcasecmp(path + strlen(path) - 4, ".log") == 0) file->echo = 1;
#endif
    handle = SRW_ObjectCreate(&file->object);
    if (handle == 0)
    {
        destroy_file(&file->object);
        free(file);
        SRW_SetLastError(4); /* too many open files */
        return INVALID_HANDLE_VALUE;
    }
    if (dwCreationDisposition == 2 || dwCreationDisposition == 4)
    {
        /* an existing file that was opened or replaced reports it through the last error */
        SRW_SetLastError(0);
    }
    return handle;
}

static uint32_t read_file(FileObject *object, void *lpBuffer, uint32_t nNumberOfBytesToRead, void *lpNumberOfBytesRead, void *lpOverlapped)
{
    int fd = (object != NULL) ? object->fd : -1;
    uint8_t *buffer = lpBuffer;
    uint32_t total = 0;

    uint32_t *overlapped = lpOverlapped;   /* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent */

    if (lpNumberOfBytesRead != NULL) *(uint32_t *) lpNumberOfBytesRead = 0;
    if (object == NULL) return 0;
    if (overlapped != NULL)
    {
        /* a program may read a file this way: the position is in the structure, not the file's. The read is done at once
           (as Windows does when the data is at hand), the structure says how much, and the event is set */
        uint64_t offset = ((uint64_t) overlapped[3] << 32) | overlapped[2];
        uint32_t got32 = 0;

        if (object->remote != NULL)
        {
            int64_t got = SRW_RemoteRead(object->remote, offset, buffer, nNumberOfBytesToRead);

            if (got < 0)
            {
                trace_remote("failed read of", object, offset, nNumberOfBytesToRead);
                overlapped[0] = 0xc0000001u;
                SRW_SetLastError(got == SRW_REMOTE_MISSING ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
                return 0;
            }
            trace_remote("read", object, offset, (uint64_t) got);
            got32 = (uint32_t) got;
        }
        else
        {
            while (got32 < nNumberOfBytesToRead)
            {
                ssize_t got = pread(fd, buffer + got32, nNumberOfBytesToRead - got32, (off_t) (offset + got32));

                if (got < 0)
                {
                    if (errno == EINTR) continue;
                    overlapped[0] = 0xc0000001u;
                    SRW_SetLastError(error_from_errno(errno));
                    return 0;
                }
                if (got == 0) break;
                got32 += (uint32_t) got;
            }
        }
        overlapped[0] = 0;
        overlapped[1] = got32;
        if (lpNumberOfBytesRead != NULL) *(uint32_t *) lpNumberOfBytesRead = got32;
        if (overlapped[4] != 0) SetEvent_c(overlapped[4]);
        SRW_SetLastError(0);
        return 1;
    }
    if (object->remote != NULL)
    {
        int64_t got = SRW_RemoteRead(object->remote, object->position, buffer, nNumberOfBytesToRead);

        if (got < 0)
        {
            trace_remote("failed read of", object, object->position, nNumberOfBytesToRead);
            SRW_SetLastError(got == SRW_REMOTE_MISSING ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
            return 0;
        }
        trace_remote("read", object, object->position, (uint64_t) got);
        object->position += (uint64_t) got;
        if (lpNumberOfBytesRead != NULL) *(uint32_t *) lpNumberOfBytesRead = (uint32_t) got;
        return 1;
    }

    while (total < nNumberOfBytesToRead)
    {
        ssize_t got = read(fd, buffer + total, nNumberOfBytesToRead - total);

        if (got < 0)
        {
            if (errno == EINTR) continue;
            SRW_SetLastError(error_from_errno(errno));
            return 0;
        }
        if (got == 0) break;
        total += (uint32_t) got;
    }
    if (lpNumberOfBytesRead != NULL) *(uint32_t *) lpNumberOfBytesRead = total;
    return 1;
}

uint32_t CCALL ReadFile_c(uint32_t hFile, void *lpBuffer, uint32_t nNumberOfBytesToRead, void *lpNumberOfBytesRead, void *lpOverlapped)
{
    FileObject *object = file_ref(hFile);
    uint32_t result = read_file(object, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);

    file_unref(object);
    return result;
}

static uint32_t write_file(FileObject *object, void *lpBuffer, uint32_t nNumberOfBytesToWrite, void *lpNumberOfBytesWritten, void *lpOverlapped)
{
    int fd = fd_of(object);
    const uint8_t *buffer = lpBuffer;
    uint32_t total = 0;

    (void) lpOverlapped;

    if (lpNumberOfBytesWritten != NULL) *(uint32_t *) lpNumberOfBytesWritten = 0;
    if (fd < 0) return 0;
#ifdef SRW_DIAGNOSTICS
    {
        static int lines;

        if (object->echo && lines < 400)
        {
            char text[600];
            size_t n = nNumberOfBytesToWrite < sizeof(text) - 1 ? nNumberOfBytesToWrite : sizeof(text) - 1;

            memcpy(text, buffer, n);
            while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
            text[n] = 0;
            lines++;
            if (n > 0)
            {
                char *line = text, *end;

                do
                {
                    end = strchr(line, '\n');
                    if (end != NULL) *end = 0;
                    if (*line != 0) logprintf("%s", line);
                    line = end + 1;
                } while (end != NULL);
            }
        }
    }
#endif

    while (total < nNumberOfBytesToWrite)
    {
        ssize_t put = write(fd, buffer + total, nNumberOfBytesToWrite - total);

        if (put < 0)
        {
            if (errno == EINTR) continue;
            SRW_SetLastError(error_from_errno(errno));
            return 0;
        }
        total += (uint32_t) put;
    }
    if (lpNumberOfBytesWritten != NULL) *(uint32_t *) lpNumberOfBytesWritten = total;
    return 1;
}

uint32_t CCALL WriteFile_c(uint32_t hFile, void *lpBuffer, uint32_t nNumberOfBytesToWrite, void *lpNumberOfBytesWritten, void *lpOverlapped)
{
    FileObject *object = file_ref(hFile);
    uint32_t result = write_file(object, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);

    file_unref(object);
    return result;
}

static uint32_t set_file_pointer(FileObject *object, uint32_t lDistanceToMove, void *lpDistanceToMoveHigh, uint32_t dwMoveMethod)
{
    int fd = (object != NULL) ? object->fd : -1;
    int64_t distance;
    off_t position;
    uint32_t *high = lpDistanceToMoveHigh;

    if (object == NULL) return INVALID_SET_FILE_POINTER;
    if (dwMoveMethod > 2)
    {
        SRW_SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }
    distance = (high != NULL) ? (int64_t) (((uint64_t) *high << 32) | lDistanceToMove) : (int64_t) (int32_t) lDistanceToMove;

    if (object->remote != NULL)
    {
        int64_t base = (dwMoveMethod == 0) ? 0 : (dwMoveMethod == 1) ? (int64_t) object->position : (int64_t) SRW_RemoteSize(object->remote);
        int64_t target = base + distance;

        if (target < 0)
        {
            SRW_SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_SET_FILE_POINTER;
        }
        trace_remote("seek", object, (uint64_t) distance, dwMoveMethod);
        object->position = (uint64_t) target;
        if (high != NULL) *high = (uint32_t) ((uint64_t) target >> 32);
        SRW_SetLastError(0);
        return (uint32_t) target;
    }
    position = lseek(fd, (off_t) distance, (dwMoveMethod == 0) ? SEEK_SET : (dwMoveMethod == 1) ? SEEK_CUR : SEEK_END);
    if (position < 0)
    {
        SRW_SetLastError(error_from_errno(errno));
        return INVALID_SET_FILE_POINTER;
    }
    if (high != NULL) *high = (uint32_t) ((uint64_t) position >> 32);
    SRW_SetLastError(0);
    return (uint32_t) position;
}

uint32_t CCALL SetFilePointer_c(uint32_t hFile, uint32_t lDistanceToMove, void *lpDistanceToMoveHigh, uint32_t dwMoveMethod)
{
    FileObject *object = file_ref(hFile);
    uint32_t result = set_file_pointer(object, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);

    file_unref(object);
    return result;
}

uint32_t CCALL GetFileSize_c(uint32_t hFile, void *lpFileSizeHigh)
{
    struct stat st;

    if (stat_handle(hFile, &st) != 0) return INVALID_SET_FILE_POINTER;
    if (lpFileSizeHigh != NULL) *(uint32_t *) lpFileSizeHigh = (uint32_t) ((uint64_t) st.st_size >> 32);
    SRW_SetLastError(0);
    return (uint32_t) st.st_size;
}

uint32_t CCALL GetFileTime_c(uint32_t hFile, void *lpCreationTime, void *lpLastAccessTime, void *lpLastWriteTime)
{
    struct stat st;

    if (stat_handle(hFile, &st) != 0) return 0;
    if (lpCreationTime != NULL) make_file_time(st.st_ctime, lpCreationTime);
    if (lpLastAccessTime != NULL) make_file_time(st.st_atime, lpLastAccessTime);
    if (lpLastWriteTime != NULL) make_file_time(st.st_mtime, lpLastWriteTime);
    return 1;
}

uint32_t CCALL SetFileTime_c(uint32_t hFile, void *lpCreationTime, void *lpLastAccessTime, void *lpLastWriteTime)
{
    (void) lpCreationTime;
    (void) lpLastAccessTime;
    (void) lpLastWriteTime;
    return file_has_fd(hFile);
}

uint32_t CCALL GetFileInformationByHandle_c(uint32_t hFile, void *lpFileInformation)
{
    struct stat st;
    FileInformation *info = lpFileInformation;

    if (stat_handle(hFile, &st) != 0) return 0;
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = attributes_of(&st);
    make_file_time(st.st_ctime, &info->ftCreationTime);
    make_file_time(st.st_atime, &info->ftLastAccessTime);
    make_file_time(st.st_mtime, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = 0x57305741;
    info->nFileSizeHigh = (uint32_t) ((uint64_t) st.st_size >> 32);
    info->nFileSizeLow = (uint32_t) st.st_size;
    info->nNumberOfLinks = 1;
    info->nFileIndexLow = (uint32_t) st.st_ino;
    return 1;
}

uint32_t CCALL SetEndOfFile_c(uint32_t hFile)
{
    FileObject *object = file_ref(hFile);
    int fd = fd_of(object), ok = 0;
    off_t position;

    if (fd >= 0)
    {
        position = lseek(fd, 0, SEEK_CUR);
        ok = position >= 0 && ftruncate(fd, position) == 0;
        if (!ok) SRW_SetLastError(ERROR_ACCESS_DENIED);
    }
    file_unref(object);
    return ok;
}

uint32_t CCALL FlushFileBuffers_c(uint32_t hFile)
{
    return file_has_fd(hFile);
}

uint32_t CCALL PeekNamedPipe_c(uint32_t hNamedPipe, void *lpBuffer, uint32_t nBufferSize, void *lpBytesRead, void *lpTotalBytesAvail, void *lpBytesLeftThisMessage)
{
    (void) hNamedPipe;
    (void) lpBuffer;
    (void) nBufferSize;
    (void) lpBytesRead;
    (void) lpTotalBytesAvail;
    (void) lpBytesLeftThisMessage;
    SRW_SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* names */

uint32_t CCALL GetFileAttributesA_c(const char *lpFileName)
{
    char path[520];
    struct stat st;

    SRW_MapPath(lpFileName, path, sizeof(path));
    SRW_LOG(SRW_LOG_FILES, "GetFileAttributes(\"%s\" -> \"%s\")", lpFileName, path);
    if (stat(path, &st) != 0)
    {
        if (SRW_RemoteFind(path) != NULL) return 0x20; /* FILE_ATTRIBUTE_ARCHIVE */
        if (SRW_RemoteIsDirectory(path)) return FILE_ATTRIBUTE_DIRECTORY;
        SRW_LOG(SRW_LOG_FILES, "... not found");
        SRW_SetLastError(error_from_errno(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes_of(&st);
}

uint32_t CCALL CreateDirectoryA_c(const char *lpPathName, void *lpSecurityAttributes)
{
    char path[520];

    (void) lpSecurityAttributes;

    SRW_MapPath(lpPathName, path, sizeof(path));
    if (mkdir(path, 0777) != 0)
    {
        SRW_SetLastError((errno == EEXIST) ? ERROR_ALREADY_EXISTS : error_from_errno(errno));
        return 0;
    }
    return 1;
}

uint32_t CCALL DeleteFileA_c(const char *lpFileName)
{
    char path[520];

    SRW_MapPath(lpFileName, path, sizeof(path));
    if (unlink(path) != 0)
    {
        SRW_SetLastError(error_from_errno(errno));
        return 0;
    }
    return 1;
}

/* the guest view of a mapped path: the install directory is C:\name */
static uint32_t windows_path(const char *mapped, char *out, uint32_t size)
{
    char text[520];
    uint32_t length, i;

    mapped = SRW_GameRelative(mapped);
    snprintf(text, sizeof(text), "%s%s", SRW_INSTALL_DIR, (mapped[0] == '/' && mapped[1] == 0) ? "" : mapped);
    length = (uint32_t) strlen(text);
    for (i = 0; i < length; i++)
    {
        if (text[i] == '/') text[i] = '\\';
    }
    if (size <= length) return length + 1;
    memcpy(out, text, length + 1);
    return length;
}

uint32_t CCALL GetCurrentDirectoryA_c(uint32_t nBufferLength, void *lpBuffer)
{
    char mapped[260];

    SRW_GetCurrentDirectory(mapped, sizeof(mapped));
    return windows_path(mapped, lpBuffer, nBufferLength);
}

uint32_t CCALL SetCurrentDirectoryA_c(const char *lpPathName)
{
    char path[520];
    struct stat st;

    SRW_MapPath(lpPathName, path, sizeof(path));
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        SRW_SetLastError(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    SRW_SetCurrentDirectory(path);
    return 1;
}

uint32_t CCALL GetFullPathNameA_c(const char *lpFileName, uint32_t nBufferLength, void *lpBuffer, void *lpFilePart)
{
    char path[520];
    uint32_t length;

    SRW_MapPath(lpFileName, path, sizeof(path));
    length = windows_path(path, lpBuffer, nBufferLength);
    SRW_LOG(SRW_LOG_FILES, "GetFullPathName(\"%s\") = \"%s\"", lpFileName, path);
    if (lpFilePart != NULL)
    {
        char *buffer = lpBuffer;
        char *part = (length < nBufferLength) ? strrchr(buffer, '\\') : NULL;

        *(uint32_t *) lpFilePart = (part != NULL) ? (uint32_t) (uintptr_t) (part + 1) : 0;
    }
    return length;
}

static uint32_t copy_string(const char *text, void *buffer, uint32_t size)
{
    uint32_t length = (uint32_t) strlen(text);

    if (size <= length) return length + 1;
    memcpy(buffer, text, length + 1);
    return length;
}

uint32_t CCALL GetWindowsDirectoryA_c(void *lpBuffer, uint32_t uSize)
{
    return copy_string("C:\\WINDOWS", lpBuffer, uSize);
}

uint32_t CCALL GetSystemDirectoryA_c(void *lpBuffer, uint32_t uSize)
{
    return copy_string("C:\\WINDOWS\\SYSTEM32", lpBuffer, uSize);
}

uint32_t CCALL GetTempPathA_c(uint32_t nBufferLength, void *lpBuffer)
{
    return copy_string("C:\\TEMP\\", lpBuffer, nBufferLength);
}

uint32_t CCALL GetDriveTypeA_c(const char *lpRootPathName)
{
    (void) lpRootPathName;
    return 3; /* DRIVE_FIXED */
}

uint32_t CCALL GetDiskFreeSpaceA_c(const char *lpRootPathName, void *lpSectorsPerCluster, void *lpBytesPerSector, void *lpNumberOfFreeClusters, void *lpTotalNumberOfClusters)
{
    (void) lpRootPathName;
    if (lpSectorsPerCluster != NULL) *(uint32_t *) lpSectorsPerCluster = 8;
    if (lpBytesPerSector != NULL) *(uint32_t *) lpBytesPerSector = 512;
    if (lpNumberOfFreeClusters != NULL) *(uint32_t *) lpNumberOfFreeClusters = 0x400000;
    if (lpTotalNumberOfClusters != NULL) *(uint32_t *) lpTotalNumberOfClusters = 0x800000;
    return 1;
}

uint32_t CCALL GetVolumeInformationA_c(const char *lpRootPathName, void *lpVolumeNameBuffer, uint32_t nVolumeNameSize,
                                       void *lpVolumeSerialNumber, void *lpMaximumComponentLength, void *lpFileSystemFlags,
                                       void *lpFileSystemNameBuffer, uint32_t nFileSystemNameSize)
{
    (void) lpRootPathName;
    if (lpVolumeNameBuffer != NULL && nVolumeNameSize > 0) copy_string(SRW_PROGRAM_NAME, lpVolumeNameBuffer, nVolumeNameSize);
    if (lpVolumeSerialNumber != NULL) *(uint32_t *) lpVolumeSerialNumber = 0x57305741;
    if (lpMaximumComponentLength != NULL) *(uint32_t *) lpMaximumComponentLength = 255;
    if (lpFileSystemFlags != NULL) *(uint32_t *) lpFileSystemFlags = 3; /* case preserving and sensitive search */
    if (lpFileSystemNameBuffer != NULL && nFileSystemNameSize > 0) copy_string("NTFS", lpFileSystemNameBuffer, nFileSystemNameSize);
    return 1;
}

uint32_t CCALL GetComputerNameA_c(void *lpBuffer, void *nSize)
{
    uint32_t *size = nSize;
    uint32_t length = 6;

    if (*size <= length)
    {
        *size = length + 1;
        SRW_SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(lpBuffer, "SRWWEB", length + 1);
    *size = length;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* directory searches */

typedef struct {
    SRW_Object object;
    FindData *entries;
    uint32_t count;
    uint32_t next;
} FindObject;

static void destroy_find(SRW_Object *object)
{
    free(((FindObject *) object)->entries);
}

/* * and ? only, without regard to case; "*.*" matches every name as it does on Windows */
static int wildcard_match(const char *pattern, const char *name)
{
    if (strcmp(pattern, "*.*") == 0) return 1;
    for (; *pattern != 0; pattern++)
    {
        if (*pattern == '*')
        {
            while (pattern[1] == '*') pattern++;
            if (pattern[1] == 0) return 1;
            for (; *name != 0; name++)
            {
                if (wildcard_match(pattern + 1, name)) return 1;
            }
            return wildcard_match(pattern + 1, name);
        }
        if (*name == 0) return 0;
        if (*pattern != '?' && tolower((unsigned char) *pattern) != tolower((unsigned char) *name)) return 0;
        name++;
    }
    return *name == 0;
}

static void fill_find_data(FindData *data, const char *directory, const char *name)
{
    char full[1100];
    struct stat st;

    memset(data, 0, sizeof(*data));
    snprintf(full, sizeof(full), "%s/%s", directory, name);
    if (stat(full, &st) == 0)
    {
        data->dwFileAttributes = attributes_of(&st);
        make_file_time(st.st_ctime, &data->ftCreationTime);
        make_file_time(st.st_atime, &data->ftLastAccessTime);
        make_file_time(st.st_mtime, &data->ftLastWriteTime);
        if (!S_ISDIR(st.st_mode))
        {
            data->nFileSizeHigh = (uint32_t) ((uint64_t) st.st_size >> 32);
            data->nFileSizeLow = (uint32_t) st.st_size;
        }
    }
    else
    {
        data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    strncpy(data->cFileName, name, sizeof(data->cFileName) - 1);
}

typedef struct {
    FindObject *find;
    const char *pattern;
    uint32_t capacity;
} RemoteContext;

/* adds a streamed file (or a directory holding some) to a search */
static void add_remote_entry(const char *name, int64_t size, void *data)
{
    RemoteContext *context = data;
    FindObject *find = context->find;
    FindData *entry;
    uint32_t i;

    if (!wildcard_match(context->pattern, name)) return;
    for (i = 0; i < find->count; i++)
    {
        if (strcasecmp(find->entries[i].cFileName, name) == 0) return;
    }
    if (find->count == context->capacity)
    {
        FindData *grown = realloc(find->entries, context->capacity * 2 * sizeof(FindData));

        if (grown == NULL) return;
        find->entries = grown;
        context->capacity *= 2;
    }
    entry = &find->entries[find->count++];
    memset(entry, 0, sizeof(*entry));
    entry->dwFileAttributes = (size < 0) ? FILE_ATTRIBUTE_DIRECTORY : 0x20;
    if (size > 0)
    {
        entry->nFileSizeHigh = (uint32_t) ((uint64_t) size >> 32);
        entry->nFileSizeLow = (uint32_t) size;
    }
    make_file_time(1130000000, &entry->ftCreationTime);
    make_file_time(1130000000, &entry->ftLastAccessTime);
    make_file_time(1130000000, &entry->ftLastWriteTime);
    strncpy(entry->cFileName, name, sizeof(entry->cFileName) - 1);
}

uint32_t SRW_LastCaller(void);

uint32_t CCALL FindFirstFileA_c(const char *lpFileName, void *lpFindFileData)
{
    char path[520], *slash, pattern_text[260];
    const char *pattern;
    DIR *dir;
    struct dirent *entry;
    FindObject *find;
    uint32_t capacity = 64, handle;

    SRW_MapPath(lpFileName, path, sizeof(path));
    SRW_LOG(SRW_LOG_FILES, "FindFirstFile(\"%s\" -> \"%s\")", lpFileName, path);
    slash = strrchr(path, '/');
    if (slash == NULL) return INVALID_HANDLE_VALUE;
    snprintf(pattern_text, sizeof(pattern_text), "%s", slash + 1);
    pattern = pattern_text;
    if (slash == path) path[1] = 0; /* the root */
    else *slash = 0;

    dir = opendir(path);
    if (dir == NULL && !SRW_RemoteIsDirectory(path))
    {
        SRW_LOG(SRW_LOG_FILES, "FindFirstFile(\"%s\") found no directory", path);
        SRW_SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    find = calloc(1, sizeof(*find));
    if (find == NULL)
    {
        closedir(dir);
        return INVALID_HANDLE_VALUE;
    }
    find->entries = malloc(capacity * sizeof(FindData));
    if (find->entries == NULL)
    {
        closedir(dir);
        free(find);
        return INVALID_HANDLE_VALUE;
    }
    while (dir != NULL && (entry = readdir(dir)) != NULL)
    {
        if (!wildcard_match(pattern, entry->d_name)) continue;
        if (find->count == capacity)
        {
            FindData *grown = realloc(find->entries, capacity * 2 * sizeof(FindData));

            if (grown == NULL) break;
            find->entries = grown;
            capacity *= 2;
        }
        fill_find_data(&find->entries[find->count++], path, entry->d_name);
    }
    if (dir != NULL) closedir(dir);
    {
        RemoteContext context = {find, pattern, capacity};

        SRW_RemoteList(path, add_remote_entry, &context);
        capacity = context.capacity;
    }

    if (find->count == 0)
    {
        SRW_LOG(SRW_LOG_FILES, "FindFirstFile(\"%s\" = \"%s\" in \"%s\") found nothing", lpFileName, pattern, path);
        free(find->entries);
        free(find);
        SRW_SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    find->object.type = SRW_OBJ_FIND;
    find->object.destroy = destroy_find;
    handle = SRW_ObjectCreate(&find->object);
    if (handle == 0)
    {
        destroy_find(&find->object);
        free(find);
        return INVALID_HANDLE_VALUE;
    }
    SRW_LOG(SRW_LOG_FILES, "FindFirstFile: %u entries, called from %x", (unsigned int) find->count, (unsigned int) SRW_LastCaller());
    for (uint32_t k = 0; k < find->count && k < 200; k++)
        SRW_LOG(SRW_LOG_FILES, "  entry %u: \"%s\" attributes %x size %x:%x", (unsigned int) k, (const char *) &find->entries[k] + 44,
                (unsigned int) ((const uint32_t *) &find->entries[k])[0], (unsigned int) ((const uint32_t *) &find->entries[k])[7], (unsigned int) ((const uint32_t *) &find->entries[k])[8]);
    memcpy(lpFindFileData, &find->entries[0], sizeof(FindData));
    find->next = 1;
    return handle;
}

uint32_t CCALL FindNextFileA_c(uint32_t hFindFile, void *lpFindFileData)
{
    FindObject *find = (FindObject *) SRW_ObjectRef(hFindFile, SRW_OBJ_FIND);
    uint32_t found = 0;

    if (find == NULL)
    {
        SRW_SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (find->next >= find->count) SRW_SetLastError(ERROR_NO_MORE_FILES);
    else
    {
        memcpy(lpFindFileData, &find->entries[find->next++], sizeof(FindData));
        found = 1;
    }
    SRW_ObjectUnref((SRW_Object *) find);
    return found;
}

uint32_t CCALL FindClose_c(uint32_t hFindFile)
{
    if (!SRW_ObjectClose(hFindFile))
    {
        SRW_SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* file mappings: a view is a copy of the range, written back when it is unmapped if it was writable */

typedef struct {
    SRW_Object object;
    int fd;
    SRW_RemoteFile *remote;   /* the mapping of a streamed file (a small one): the views are copies of the data */
    uint32_t protect;
    uint64_t size;
} MappingObject;

static void destroy_mapping(SRW_Object *object)
{
    int fd = ((MappingObject *) object)->fd;

    if (fd >= 0) close(fd);
}

typedef struct {
    void *address;
    int fd;
    int writable;
    uint64_t offset;
    uint32_t size;
} View;

#define MAX_VIEWS 64
static View views[MAX_VIEWS];
static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t CCALL CreateFileMappingA_c(uint32_t hFile, void *lpFileMappingAttributes, uint32_t flProtect, uint32_t dwMaximumSizeHigh,
                                    uint32_t dwMaximumSizeLow, const char *lpName)
{
    MappingObject *mapping;
    struct stat st;
    uint32_t handle;
    int fd = -1;
    SRW_RemoteFile *remote = NULL;

    (void) lpFileMappingAttributes;
    (void) lpName;

    if (hFile != INVALID_HANDLE_VALUE)
    {
        FileObject *file = file_ref(hFile);

        if (file == NULL) return 0;
        if (file->fd < 0 && file->remote != NULL)
        {
            /* a streamed file: small files (the pages of the license agreement...) can be mapped, the archives cannot */
            if (SRW_RemoteSize(file->remote) > 64u * 1024 * 1024)
            {
                SRW_SetLastError(ERROR_ACCESS_DENIED);
                file_unref(file);
                return 0;
            }
            remote = file->remote;   /* (the streamed files of the manifest stay for good) */
            SRW_LOG(SRW_LOG_FILES, "Mapping of the streamed file %s", SRW_RemotePath(remote));
        }
        else
        {
            int source = fd_of(file);

            if (source >= 0) fd = dup(source);
            if (source < 0)
            {
                file_unref(file);
                return 0;
            }
        }
        file_unref(file);
    }
    mapping = calloc(1, sizeof(*mapping));
    if (mapping == NULL) return 0;
    mapping->remote = remote;
    if (remote != NULL) mapping->size = SRW_RemoteSize(remote);
    mapping->object.type = SRW_OBJ_MAPPING;
    mapping->object.destroy = destroy_mapping;
    mapping->fd = fd;
    mapping->protect = flProtect;
    if (dwMaximumSizeHigh != 0 || dwMaximumSizeLow != 0) mapping->size = ((uint64_t) dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
    if (mapping->size == 0 && fd >= 0 && fstat(fd, &st) == 0) mapping->size = (uint64_t) st.st_size;

    handle = SRW_ObjectCreate(&mapping->object);
    if (handle == 0)
    {
        destroy_mapping(&mapping->object);
        free(mapping);
    }
    return handle;
}

static uint32_t map_view(MappingObject *mapping, uint32_t dwDesiredAccess, uint32_t dwFileOffsetHigh, uint32_t dwFileOffsetLow, uint32_t dwNumberOfBytesToMap)
{
    uint64_t offset = ((uint64_t) dwFileOffsetHigh << 32) | dwFileOffsetLow;
    uint32_t size;
    uint8_t *memory;
    int i;

    if (mapping == NULL)
    {
        SRW_SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    size = dwNumberOfBytesToMap;
    if (size == 0)
    {
        if (mapping->size <= offset) return 0;
        size = (uint32_t) (mapping->size - offset);
    }
    memory = calloc(1, size);
    if (memory == NULL)
    {
        SRW_SetLastError(8);
        return 0;
    }
    if (mapping->remote != NULL)
    {
        uint32_t done = 0;
        int64_t failure = 0;

        while (done < size)
        {
            int64_t got = SRW_RemoteRead(mapping->remote, offset + done, memory + done, size - done);

            if (got < 0) failure = got;
            if (got <= 0) break;
            done += (uint32_t) got;
        }
        if (failure != 0)
        {
            /* a fill that failed (a missing file, or a download that failed) is not a view of zeros: the call fails */
            free(memory);
            SRW_SetLastError(failure == SRW_REMOTE_MISSING ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
            return 0;
        }
    }
    else if (mapping->fd >= 0)
    {
        uint32_t done = 0;

        while (done < size)
        {
            ssize_t got = pread(mapping->fd, memory + done, size - done, (off_t) (offset + done));

            if (got <= 0) break;
            done += (uint32_t) got;
        }
    }
    pthread_mutex_lock(&view_lock);
    for (i = 0; i < MAX_VIEWS; i++)
    {
        if (views[i].address == NULL)
        {
            views[i].address = memory;
            views[i].fd = mapping->fd >= 0 ? dup(mapping->fd) : -1;
            views[i].writable = (dwDesiredAccess & 2) != 0; /* FILE_MAP_WRITE */
            views[i].offset = offset;
            views[i].size = size;
            break;
        }
    }
    pthread_mutex_unlock(&view_lock);
    if (i == MAX_VIEWS)
    {
        free(memory);
        return 0;
    }
    return (uint32_t) (uintptr_t) memory;
}

uint32_t CCALL MapViewOfFile_c(uint32_t hFileMappingObject, uint32_t dwDesiredAccess, uint32_t dwFileOffsetHigh, uint32_t dwFileOffsetLow, uint32_t dwNumberOfBytesToMap)
{
    MappingObject *mapping = (MappingObject *) SRW_ObjectRef(hFileMappingObject, SRW_OBJ_MAPPING);
    uint32_t result = map_view(mapping, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);

    SRW_ObjectUnref((SRW_Object *) mapping);
    return result;
}

uint32_t CCALL UnmapViewOfFile_c(void *lpBaseAddress)
{
    int i, found = 0;

    pthread_mutex_lock(&view_lock);
    for (i = 0; i < MAX_VIEWS; i++)
    {
        if (views[i].address == lpBaseAddress && lpBaseAddress != NULL)
        {
            if (views[i].writable && views[i].fd >= 0)
            {
                if (pwrite(views[i].fd, views[i].address, views[i].size, (off_t) views[i].offset) < 0) {}
            }
            if (views[i].fd >= 0) close(views[i].fd);
            free(views[i].address);
            memset(&views[i], 0, sizeof(views[i]));
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&view_lock);
    if (!found) SRW_SetLastError(ERROR_INVALID_PARAMETER);
    return found;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the environment */

uint32_t CCALL GetEnvironmentVariableA_c(const char *lpName, void *lpBuffer, uint32_t nSize)
{
    const char *value = getenv(lpName);

    if (value == NULL)
    {
        SRW_SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }
    return copy_string(value, lpBuffer, nSize);
}

uint32_t CCALL SetEnvironmentVariableA_c(const char *lpName, const char *lpValue)
{
    return (lpValue != NULL ? setenv(lpName, lpValue, 1) : unsetenv(lpName)) == 0;
}
