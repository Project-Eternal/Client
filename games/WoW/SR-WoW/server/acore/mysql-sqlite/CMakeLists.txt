# The MySQL C client API on SQLite for AzerothCore: build with
#   cmake -S server/acore/mysql-sqlite -B BUILD -DSQLITE_AMALGAMATION_DIR=DIR   (DIR has sqlite3.c and sqlite3.h, made from third_party/sqlite)
# then configure AzerothCore with  -DMYSQL_INCLUDE_DIR=server/acore/mysql-sqlite/include -DMYSQL_LIBRARY=BUILD/libmysqlsqlite.a
cmake_minimum_required(VERSION 3.16)
project(mysql_sqlite C CXX)

set(SQLITE_AMALGAMATION_DIR "" CACHE PATH "directory with sqlite3.c and sqlite3.h")
if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
  message(FATAL_ERROR "SQLITE_AMALGAMATION_DIR must contain sqlite3.c")
endif()

add_library(mysqlsqlite STATIC src/mysql_sqlite.cpp src/ddl.cpp src/updater.cpp src/config.cpp src/backup.cpp src/stream_vfs.cpp src/changelog.cpp "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
target_include_directories(mysqlsqlite PUBLIC include PRIVATE "${SQLITE_AMALGAMATION_DIR}")
target_compile_definitions(mysqlsqlite PRIVATE
  SQLITE_ENABLE_COLUMN_METADATA SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_ENABLE_MATH_FUNCTIONS SQLITE_ENABLE_UPDATE_DELETE_LIMIT SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_TEMP_STORE=2)
# Do NOT define SQLITE_DEFAULT_MEMSTATUS=0: without memory statistics sqlite3Malloc skips its round-up to 8 bytes, and on 32-bit targets (sizeof(Op) is 20)
# sqlite3VdbeMakeReady then writes up to 7 bytes past the opcode array (found with AddressSanitizer; probably the cause of the heap corruption below)
# SQLite compiled with -O2 crashes in wasm (Emscripten 3.1.29 / LLVM 16; a parse-time memory fault after the first character login of AzerothCore; -O1 and the SQLITE_DEBUG
# build are fine), so its translation unit is -O1 there; the shim itself stays as the caller builds it
if(EMSCRIPTEN)
  set_source_files_properties("${SQLITE_AMALGAMATION_DIR}/sqlite3.c" PROPERTIES COMPILE_OPTIONS "-O1")
endif()
# extra SQLite definitions for debugging (-DSQLITE_EXTRA_DEFS="SQLITE_DEBUG;SQLITE_MEMDEBUG"): assertions and allocation guards with messages that name the line
set(SQLITE_EXTRA_DEFS "" CACHE STRING "more SQLite compile definitions")
if(SQLITE_EXTRA_DEFS)
  target_compile_definitions(mysqlsqlite PRIVATE ${SQLITE_EXTRA_DEFS})
endif()
set_target_properties(mysqlsqlite PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

# the setup tool (a build step, natively): AzerothCore's databases as SQLite files from AzerothCore's own SQL (tools/setup.cpp)
if(NOT EMSCRIPTEN)
  add_executable(acore-db-setup tools/setup.cpp)
  target_link_libraries(acore-db-setup PRIVATE mysqlsqlite ${CMAKE_DL_LIBS})
  find_package(Threads REQUIRED)
  target_link_libraries(acore-db-setup PRIVATE Threads::Threads)
  target_include_directories(acore-db-setup PRIVATE "${SQLITE_AMALGAMATION_DIR}")
  target_compile_definitions(acore-db-setup PRIVATE SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK)
  set_target_properties(acore-db-setup PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endif()
