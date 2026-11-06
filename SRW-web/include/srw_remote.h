#ifndef SRW_REMOTE_H
#define SRW_REMOTE_H

#include <stdint.h>

/* The program's data files (archives of several GB) are not in the file system: a manifest lists them and their
   contents are fetched in pieces from the server with HTTP range requests, cached in memory. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SRW_RemoteFile SRW_RemoteFile;

/* the file of a path (as SRW_MapPath produces it), or NULL if it is not in the manifest */
SRW_RemoteFile *SRW_RemoteFind(const char *mapped_path);

uint64_t SRW_RemoteSize(const SRW_RemoteFile *file);

/* the path as listed in the manifest */
const char *SRW_RemotePath(const SRW_RemoteFile *file);

/* SRW_RemoteRead's result for a file the host no longer serves: the file is marked, SRW_RemoteFind, SRW_RemoteList and
   SRW_RemoteIsDirectory stop reporting it, and its reads that need a download fail at once with this result */
#define SRW_REMOTE_MISSING (-2)

/* reads up to size bytes at offset; returns the number read (short at the end of the file), -1 if the server failed, SRW_REMOTE_MISSING if the file is missing */
int64_t SRW_RemoteRead(SRW_RemoteFile *file, uint64_t offset, void *buffer, uint32_t size);
/* called by the thread that presents the frames: its reads are the ones that freeze the picture */
void SRW_RemoteNoteGameThread(void);

/* does the path name a directory that contains manifest files? */
int SRW_RemoteIsDirectory(const char *mapped_path);

/* the manifest entries directly inside a directory: calls back with the name and the size, or -1 for a subdirectory */
void SRW_RemoteList(const char *mapped_path, void (*entry)(const char *name, int64_t size, void *context), void *context);

#ifdef __cplusplus
}
#endif

#endif
