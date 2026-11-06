/* Extras of the SQLite shim beyond the MySQL client API (used by the setup tool and by the shim itself). */
#pragma once

#include "mysql.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the statements of a SQL file (MySQL syntax, as AzerothCore's dumps and updates) on the connection, in one transaction.
   skip (may be NULL) is asked for every statement and returns nonzero to leave it out. Returns 0, or 1 with the error in mysql_error(). */
int mysql_sqlite_apply_file(MYSQL* mysql, const char* path, int (*skip)(const char* statement, void* context), void* context);

/* AzerothCore's database updater (DBUpdater/UpdateFetcher with its default settings: Redundancy 1, AllowRehash 1, ArchivedRedundancy 0) without the
   mysql command line program: the directories of `updates_include` ($ = source), their .sql files in name order (released and archived ones first,
   then pending and custom ones), applied when the `updates` table does not have them or has another hash; every applied file is recorded there.
   Prints what it applies to stderr. Returns the number of applied files, or -1 with the error in mysql_error(). */
int mysql_sqlite_apply_updates(MYSQL* mysql, const char* source);

#ifdef __cplusplus
}
#endif
