/* The part of the MySQL C client API that AzerothCore's database layer uses, implemented over SQLite (src/mysql_sqlite.cpp).
   Written from the public API description; no MySQL code. AzerothCore compiles its own database classes unchanged against this
   header: MYSQL_RES, MYSQL_STMT and MYSQL_BIND have the members its sources touch. */
#ifndef MYSQL_SQLITE_SHIM_H
#define MYSQL_SQLITE_SHIM_H

#include <stddef.h>

#define MYSQL_VERSION_ID 80400
#define MYSQL_SERVER_VERSION "8.4.0"

typedef unsigned long long my_ulonglong;
typedef char** MYSQL_ROW;

enum enum_field_types
{
    MYSQL_TYPE_DECIMAL, MYSQL_TYPE_TINY, MYSQL_TYPE_SHORT, MYSQL_TYPE_LONG, MYSQL_TYPE_FLOAT, MYSQL_TYPE_DOUBLE,
    MYSQL_TYPE_NULL, MYSQL_TYPE_TIMESTAMP, MYSQL_TYPE_LONGLONG, MYSQL_TYPE_INT24, MYSQL_TYPE_DATE, MYSQL_TYPE_TIME,
    MYSQL_TYPE_DATETIME, MYSQL_TYPE_YEAR, MYSQL_TYPE_NEWDATE, MYSQL_TYPE_VARCHAR, MYSQL_TYPE_BIT,
    MYSQL_TYPE_NEWDECIMAL = 246, MYSQL_TYPE_ENUM = 247, MYSQL_TYPE_SET = 248, MYSQL_TYPE_TINY_BLOB = 249,
    MYSQL_TYPE_MEDIUM_BLOB = 250, MYSQL_TYPE_LONG_BLOB = 251, MYSQL_TYPE_BLOB = 252, MYSQL_TYPE_VAR_STRING = 253,
    MYSQL_TYPE_STRING = 254, MYSQL_TYPE_GEOMETRY = 255
};

#define NOT_NULL_FLAG 1
#define PRI_KEY_FLAG 2
#define UNIQUE_KEY_FLAG 4
#define MULTIPLE_KEY_FLAG 8
#define BLOB_FLAG 16
#define UNSIGNED_FLAG 32
#define ZEROFILL_FLAG 64
#define BINARY_FLAG 128
#define AUTO_INCREMENT_FLAG 512

#define MYSQL_NO_DATA 100
#define MYSQL_DATA_TRUNCATED 101

enum mysql_option
{
    MYSQL_OPT_CONNECT_TIMEOUT, MYSQL_OPT_COMPRESS, MYSQL_OPT_NAMED_PIPE, MYSQL_INIT_COMMAND, MYSQL_READ_DEFAULT_FILE,
    MYSQL_READ_DEFAULT_GROUP, MYSQL_SET_CHARSET_DIR, MYSQL_SET_CHARSET_NAME, MYSQL_OPT_LOCAL_INFILE, MYSQL_OPT_PROTOCOL,
    MYSQL_SHARED_MEMORY_BASE_NAME, MYSQL_OPT_READ_TIMEOUT, MYSQL_OPT_WRITE_TIMEOUT, MYSQL_OPT_SSL_MODE
};
enum mysql_protocol_type { MYSQL_PROTOCOL_DEFAULT, MYSQL_PROTOCOL_TCP, MYSQL_PROTOCOL_SOCKET, MYSQL_PROTOCOL_PIPE, MYSQL_PROTOCOL_MEMORY };
enum mysql_ssl_mode { SSL_MODE_DISABLED = 1, SSL_MODE_PREFERRED, SSL_MODE_REQUIRED };
enum enum_stmt_attr_type { STMT_ATTR_UPDATE_MAX_LENGTH, STMT_ATTR_CURSOR_TYPE, STMT_ATTR_PREFETCH_ROWS };

struct st_mysql_field
{
    char* name;
    char* org_name;
    char* table;
    char* org_table;
    char* db;
    char* catalog;
    char* def;
    unsigned long length;
    unsigned long max_length;
    unsigned int name_length, org_name_length, table_length, org_table_length, db_length, catalog_length, def_length;
    unsigned int flags;
    unsigned int decimals;
    unsigned int charsetnr;
    enum enum_field_types type;
    void* extension;
};
typedef struct st_mysql_field MYSQL_FIELD;

typedef struct st_mysql_time
{
    unsigned int year, month, day, hour, minute, second;
    unsigned long second_part;
    bool neg;
    int time_type;
} MYSQL_TIME;

struct st_mysql;
typedef struct st_mysql MYSQL;
struct ShimResult;
struct ShimStmt;
struct ShimMysql;

struct st_mysql_res
{
    MYSQL* handle;
    ShimResult* impl;
};
typedef struct st_mysql_res MYSQL_RES;

struct st_mysql_bind
{
    unsigned long* length;
    bool* is_null;
    void* buffer;
    bool* error;
    unsigned char* row_ptr;
    void (*store_param_func)(void*, struct st_mysql_bind*);
    void (*fetch_result)(struct st_mysql_bind*, MYSQL_FIELD*, unsigned char**);
    void (*skip_result)(struct st_mysql_bind*, MYSQL_FIELD*, unsigned char**);
    unsigned long buffer_length;
    unsigned long offset;
    unsigned long length_value;
    unsigned int param_number;
    unsigned int pack_length;
    enum enum_field_types buffer_type;
    bool error_value;
    bool is_unsigned;
    bool long_data_used;
    bool is_null_value;
    void* extension;
};
typedef struct st_mysql_bind MYSQL_BIND;

struct st_mysql_stmt
{
    MYSQL* mysql;
    MYSQL_BIND* params;
    MYSQL_BIND* bind;
    unsigned long param_count;
    unsigned long field_count;
    bool bind_result_done;
    ShimStmt* impl;
};
typedef struct st_mysql_stmt MYSQL_STMT;

struct st_mysql
{
    ShimMysql* impl;
};

#ifdef __cplusplus
extern "C" {
#endif

int mysql_library_init(int argc, char** argv, char** groups);
void mysql_library_end(void);
unsigned int mysql_thread_safe(void);
unsigned long mysql_get_client_version(void);
const char* mysql_get_client_info(void);

MYSQL* mysql_init(MYSQL* mysql);
int mysql_options(MYSQL* mysql, enum mysql_option option, const void* arg);
MYSQL* mysql_real_connect(MYSQL* mysql, const char* host, const char* user, const char* passwd, const char* db,
                          unsigned int port, const char* unix_socket, unsigned long clientflag);
void mysql_close(MYSQL* mysql);
int mysql_ping(MYSQL* mysql);
int mysql_autocommit(MYSQL* mysql, bool mode);
int mysql_set_character_set(MYSQL* mysql, const char* csname);
unsigned int mysql_errno(MYSQL* mysql);
const char* mysql_error(MYSQL* mysql);
unsigned long mysql_get_server_version(MYSQL* mysql);
const char* mysql_get_server_info(MYSQL* mysql);
unsigned long mysql_real_escape_string(MYSQL* mysql, char* to, const char* from, unsigned long length);

int mysql_query(MYSQL* mysql, const char* q);
MYSQL_RES* mysql_store_result(MYSQL* mysql);
my_ulonglong mysql_affected_rows(MYSQL* mysql);
unsigned int mysql_field_count(MYSQL* mysql);
bool mysql_more_results(MYSQL* mysql);
int mysql_next_result(MYSQL* mysql);
MYSQL_ROW mysql_fetch_row(MYSQL_RES* result);
unsigned long* mysql_fetch_lengths(MYSQL_RES* result);
MYSQL_FIELD* mysql_fetch_fields(MYSQL_RES* result);
void mysql_free_result(MYSQL_RES* result);

MYSQL_STMT* mysql_stmt_init(MYSQL* mysql);
int mysql_stmt_prepare(MYSQL_STMT* stmt, const char* query, unsigned long length);
unsigned long mysql_stmt_param_count(MYSQL_STMT* stmt);
int mysql_stmt_attr_set(MYSQL_STMT* stmt, enum enum_stmt_attr_type attr_type, const void* attr);
bool mysql_stmt_bind_param(MYSQL_STMT* stmt, MYSQL_BIND* bnd);
bool mysql_stmt_bind_named_param(MYSQL_STMT* stmt, MYSQL_BIND* bnd, unsigned int n_params, const char** names);
int mysql_stmt_execute(MYSQL_STMT* stmt);
MYSQL_RES* mysql_stmt_result_metadata(MYSQL_STMT* stmt);
unsigned int mysql_stmt_field_count(MYSQL_STMT* stmt);
my_ulonglong mysql_stmt_affected_rows(MYSQL_STMT* stmt);
my_ulonglong mysql_stmt_num_rows(MYSQL_STMT* stmt);
int mysql_stmt_store_result(MYSQL_STMT* stmt);
bool mysql_stmt_bind_result(MYSQL_STMT* stmt, MYSQL_BIND* bnd);
int mysql_stmt_fetch(MYSQL_STMT* stmt);
bool mysql_stmt_free_result(MYSQL_STMT* stmt);
bool mysql_stmt_close(MYSQL_STMT* stmt);
unsigned int mysql_stmt_errno(MYSQL_STMT* stmt);
const char* mysql_stmt_error(MYSQL_STMT* stmt);

#ifdef __cplusplus
}
#endif

#endif
