/* advapi32: an in-memory registry and the security calls the program makes at startup */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_win32.h"

enum {
    ERROR_SUCCESS = 0,
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_INVALID_HANDLE = 6,
    ERROR_MORE_DATA = 234,
    ERROR_NO_MORE_ITEMS = 259,
    REG_SZ = 1
};

typedef struct RegValue {
    char *name;
    uint32_t type;
    uint8_t *data;
    uint32_t size;
    struct RegValue *next;
} RegValue;

typedef struct RegNode {
    char *name;
    struct RegNode *parent;
    struct RegNode *children; /* first child */
    struct RegNode *sibling;
    RegValue *values;
} RegNode;

typedef struct {
    SRW_Object object;
    RegNode *node;
} KeyObject;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static RegNode roots[6];
static int registry_ready;

static RegNode *child_named(RegNode *parent, const char *name, size_t length)
{
    RegNode *child;

    for (child = parent->children; child != NULL; child = child->sibling)
    {
        if (strlen(child->name) == length && strncasecmp(child->name, name, length) == 0) return child;
    }
    return NULL;
}

static RegNode *make_child(RegNode *parent, const char *name, size_t length)
{
    RegNode *child = calloc(1, sizeof(*child));

    if (child == NULL) return NULL;
    child->name = strndup(name, length);
    child->parent = parent;
    child->sibling = parent->children;
    parent->children = child;
    return child;
}

/* the node of a path below start, optionally creating what is missing; *created tells if anything was made */
static RegNode *walk(RegNode *start, const char *path, int create, int *created)
{
    RegNode *node = start;

    if (created != NULL) *created = 0;
    while (path != NULL && *path != 0)
    {
        const char *end = path;
        RegNode *next;

        while (*end != 0 && *end != '\\') end++;
        if (end != path)
        {
            next = child_named(node, path, (size_t) (end - path));
            if (next == NULL)
            {
                if (!create) return NULL;
                next = make_child(node, path, (size_t) (end - path));
                if (next == NULL) return NULL;
                if (created != NULL) *created = 1;
            }
            node = next;
        }
        path = (*end == '\\') ? end + 1 : end;
    }
    return node;
}

static RegValue *find_value(RegNode *node, const char *name)
{
    RegValue *value;

    for (value = node->values; value != NULL; value = value->next)
    {
        if (strcasecmp(value->name, name) == 0) return value;
    }
    return NULL;
}

static void set_value(RegNode *node, const char *name, uint32_t type, const void *data, uint32_t size)
{
    RegValue *value = find_value(node, name);

    if (value == NULL)
    {
        value = calloc(1, sizeof(*value));
        value->name = strdup(name);
        value->next = node->values;
        node->values = value;
    }
    free(value->data);
    value->data = malloc(size ? size : 1);
    memcpy(value->data, data, size);
    value->size = size;
    value->type = type;
}

static void set_string(RegNode *node, const char *name, const char *text)
{
    set_value(node, name, REG_SZ, text, (uint32_t) strlen(text) + 1);
}

static void init_registry(void)
{
    static const char *const root_names[6] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS",
                                              "HKEY_PERFORMANCE_DATA", "HKEY_CURRENT_CONFIG"};
    RegNode *key;
    int i;

    if (registry_ready) return;
    registry_ready = 1;
    for (i = 0; i < 6; i++) roots[i].name = (char *) root_names[i];

    if (SRW_RegistryKey() != NULL)
    {
        /* where the program believes it was installed */
        key = walk(&roots[2], SRW_RegistryKey(), 1, NULL);
        set_string(key, "InstallPath", SRW_INSTALL_DIR "\\");
        set_string(key, "GamePath", SRW_PROGRAM_EXE);
        key = walk(&roots[1], SRW_RegistryKey(), 1, NULL);
        set_string(key, "InstallPath", SRW_INSTALL_DIR "\\");
    }
    key = walk(&roots[2], "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", 1, NULL);
    set_string(key, "CommonFilesDir", "C:\\Program Files\\Common Files");
}

static void destroy_key(SRW_Object *object)
{
    (void) object;
}

/* a key handle is either one of the predefined roots (0x80000000...) or an object */
static RegNode *node_of(uint32_t handle)
{
    if (handle >= 0x80000000u && handle <= 0x80000005u) return &roots[handle - 0x80000000u];
    {
        KeyObject *key = (KeyObject *) SRW_ObjectRef(handle, SRW_OBJ_REGKEY);
        RegNode *node = (key != NULL) ? key->node : NULL;

        SRW_ObjectUnref((SRW_Object *) key);   /* (the nodes stay for good: only the key object can go) */
        return node;
    }
}

static uint32_t new_key(RegNode *node, uint32_t *out)
{
    KeyObject *key = calloc(1, sizeof(*key));
    uint32_t handle;

    if (key == NULL) return 8;
    key->object.type = SRW_OBJ_REGKEY;
    key->object.destroy = destroy_key;
    key->node = node;
    handle = SRW_ObjectCreate(&key->object);
    if (handle == 0)
    {
        free(key);
        return 4;
    }
    *out = handle;
    return ERROR_SUCCESS;
}

static uint32_t open_key(uint32_t hKey, const char *subkey, uint32_t *result, int create, uint32_t *disposition)
{
    RegNode *start, *node;
    int created = 0;
    uint32_t status;

    pthread_mutex_lock(&registry_lock);
    init_registry();
    start = node_of(hKey);
    if (start == NULL)
    {
        pthread_mutex_unlock(&registry_lock);
        return ERROR_INVALID_HANDLE;
    }
    node = walk(start, subkey, create, &created);
    if (node == NULL)
    {
        pthread_mutex_unlock(&registry_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    status = new_key(node, result);
    pthread_mutex_unlock(&registry_lock);
    if (disposition != NULL) *disposition = created ? 1 : 2;
    return status;
}

uint32_t CCALL RegOpenKeyExA_c(uint32_t hKey, const char *lpSubKey, uint32_t ulOptions, uint32_t samDesired, void *phkResult)
{
    (void) ulOptions;
    (void) samDesired;
    return open_key(hKey, lpSubKey, phkResult, 0, NULL);
}

uint32_t CCALL RegOpenKeyA_c(uint32_t hKey, const char *lpSubKey, void *phkResult)
{
    return open_key(hKey, lpSubKey, phkResult, 0, NULL);
}

/* (hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition) */
uint32_t CCALL RegCreateKeyExA_c(const uint32_t *args)
{
    return open_key(args[0], (const char *) (uintptr_t) args[1], (uint32_t *) (uintptr_t) args[7], 1, (uint32_t *) (uintptr_t) args[8]);
}

uint32_t CCALL RegCloseKey_c(uint32_t hKey)
{
    if (hKey >= 0x80000000u && hKey <= 0x80000005u) return ERROR_SUCCESS;
    return SRW_ObjectClose(hKey) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

uint32_t CCALL RegFlushKey_c(uint32_t hKey)
{
    (void) hKey;
    return ERROR_SUCCESS;
}

uint32_t CCALL RegDeleteKeyA_c(uint32_t hKey, const char *lpSubKey)
{
    RegNode *start, *node, **link;
    uint32_t status = ERROR_FILE_NOT_FOUND;

    pthread_mutex_lock(&registry_lock);
    init_registry();
    start = node_of(hKey);
    node = (start != NULL) ? walk(start, lpSubKey, 0, NULL) : NULL;
    if (node != NULL && node->parent != NULL)
    {
        for (link = &node->parent->children; *link != NULL; link = &(*link)->sibling)
        {
            if (*link == node)
            {
                *link = node->sibling; /* the node stays allocated: open handles may still refer to it */
                status = ERROR_SUCCESS;
                break;
            }
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return status;
}

uint32_t CCALL RegDeleteValueA_c(uint32_t hKey, const char *lpValueName)
{
    RegNode *node;
    RegValue **link;
    uint32_t status = ERROR_FILE_NOT_FOUND;

    pthread_mutex_lock(&registry_lock);
    node = node_of(hKey);
    if (node != NULL)
    {
        for (link = &node->values; *link != NULL; link = &(*link)->next)
        {
            if (strcasecmp((*link)->name, lpValueName != NULL ? lpValueName : "") == 0)
            {
                *link = (*link)->next;
                status = ERROR_SUCCESS;
                break;
            }
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return status;
}

uint32_t CCALL RegQueryValueExA_c(uint32_t hKey, const char *lpValueName, void *lpReserved, void *lpType, void *lpData, void *lpcbData)
{
    RegNode *node;
    RegValue *value;
    uint32_t status = ERROR_SUCCESS;
    uint32_t *size = lpcbData;

    (void) lpReserved;

    pthread_mutex_lock(&registry_lock);
    init_registry();
    node = node_of(hKey);
    value = (node != NULL) ? find_value(node, lpValueName != NULL ? lpValueName : "") : NULL;
    if (node == NULL)
    {
        status = ERROR_INVALID_HANDLE;
    }
    else if (value == NULL)
    {
        status = ERROR_FILE_NOT_FOUND;
    }
    else
    {
        if (lpType != NULL) *(uint32_t *) lpType = value->type;
        if (lpData != NULL && size != NULL)
        {
            if (*size < value->size) status = ERROR_MORE_DATA;
            else memcpy(lpData, value->data, value->size);
        }
        if (size != NULL) *size = value->size;
    }
    pthread_mutex_unlock(&registry_lock);
    return status;
}

uint32_t CCALL RegSetValueExA_c(uint32_t hKey, const char *lpValueName, uint32_t Reserved, uint32_t dwType, void *lpData, uint32_t cbData)
{
    RegNode *node;

    (void) Reserved;

    pthread_mutex_lock(&registry_lock);
    node = node_of(hKey);
    if (node != NULL) set_value(node, lpValueName != NULL ? lpValueName : "", dwType, lpData, cbData);
    pthread_mutex_unlock(&registry_lock);
    return (node != NULL) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

uint32_t CCALL RegEnumKeyExA_c(uint32_t hKey, uint32_t dwIndex, void *lpName, void *lpcName, void *lpReserved, void *lpClass, void *lpcClass, void *lpftLastWriteTime)
{
    RegNode *node, *child;
    uint32_t status = ERROR_NO_MORE_ITEMS;
    uint32_t *size = lpcName;

    (void) lpReserved;
    (void) lpClass;
    (void) lpcClass;
    (void) lpftLastWriteTime;

    pthread_mutex_lock(&registry_lock);
    init_registry();
    node = node_of(hKey);
    if (node != NULL)
    {
        /* children are kept newest first; enumerate in creation order */
        uint32_t count = 0, index;

        for (child = node->children; child != NULL; child = child->sibling) count++;
        if (dwIndex < count)
        {
            index = count - 1 - dwIndex;
            for (child = node->children; index > 0; index--) child = child->sibling;
            if (*size <= strlen(child->name))
            {
                status = ERROR_MORE_DATA;
            }
            else
            {
                strcpy(lpName, child->name);
                *size = (uint32_t) strlen(child->name);
                status = ERROR_SUCCESS;
            }
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return status;
}

/* (hKey, lpClass, lpcClass, lpReserved, lpcSubKeys, lpcMaxSubKeyLen, lpcMaxClassLen, lpcValues, lpcMaxValueNameLen,
    lpcMaxValueLen, lpcSecurityDescriptor, lpftLastWriteTime) */
uint32_t CCALL RegQueryInfoKeyA_c(const uint32_t *args)
{
    RegNode *node, *child;
    RegValue *value;
    uint32_t subkeys = 0, max_sub = 0, values = 0, max_name = 0, max_data = 0;
    uint32_t **out = (uint32_t **) 0; /* only to make the casts below readable */

    (void) out;
    pthread_mutex_lock(&registry_lock);
    init_registry();
    node = node_of(args[0]);
    if (node == NULL)
    {
        pthread_mutex_unlock(&registry_lock);
        return ERROR_INVALID_HANDLE;
    }
    for (child = node->children; child != NULL; child = child->sibling)
    {
        subkeys++;
        if (strlen(child->name) > max_sub) max_sub = (uint32_t) strlen(child->name);
    }
    for (value = node->values; value != NULL; value = value->next)
    {
        values++;
        if (strlen(value->name) > max_name) max_name = (uint32_t) strlen(value->name);
        if (value->size > max_data) max_data = value->size;
    }
    pthread_mutex_unlock(&registry_lock);

#define STORE(index, v) do { if (args[index] != 0) *(uint32_t *) (uintptr_t) args[index] = (v); } while (0)
    if (args[1] != 0 && args[2] != 0) *(char *) (uintptr_t) args[1] = 0;
    STORE(2, 0);
    STORE(4, subkeys);
    STORE(5, max_sub);
    STORE(6, 0);
    STORE(7, values);
    STORE(8, max_name);
    STORE(9, max_data);
    STORE(10, 0);
#undef STORE
    if (args[11] != 0) memset((void *) (uintptr_t) args[11], 0, 8);
    return ERROR_SUCCESS;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* security: the program asks whether it runs as an administrator and hardens a few objects; everything succeeds */

uint32_t CCALL OpenProcessToken_c(uint32_t ProcessHandle, uint32_t DesiredAccess, void *TokenHandle)
{
    KeyObject *token = calloc(1, sizeof(*token));
    uint32_t handle;

    (void) ProcessHandle;
    (void) DesiredAccess;

    if (token == NULL) return 0;
    token->object.type = SRW_OBJ_REGKEY + 100; /* a token has no operations of its own */
    handle = SRW_ObjectCreate(&token->object);
    if (handle == 0)
    {
        free(token);
        return 0;
    }
    *(uint32_t *) TokenHandle = handle;
    return 1;
}

uint32_t CCALL GetTokenInformation_c(uint32_t TokenHandle, uint32_t TokenInformationClass, void *TokenInformation, uint32_t TokenInformationLength, void *ReturnLength)
{
    (void) TokenHandle;
    (void) TokenInformationClass;

    /* an empty group list / no privileges */
    if (ReturnLength != NULL) *(uint32_t *) ReturnLength = 4;
    if (TokenInformation == NULL || TokenInformationLength < 4)
    {
        SRW_SetLastError(122);
        return 0;
    }
    memset(TokenInformation, 0, TokenInformationLength < 64 ? TokenInformationLength : 64);
    return 1;
}

/* (pIdentifierAuthority, nSubAuthorityCount, nSubAuthority0..7, pSid) */
uint32_t CCALL AllocateAndInitializeSid_c(const uint32_t *args)
{
    uint8_t *sid = calloc(1, 32);

    if (sid == NULL) return 0;
    sid[0] = 1;
    sid[1] = (uint8_t) args[1];
    *(uint32_t *) (uintptr_t) args[10] = (uint32_t) (uintptr_t) sid;
    return 1;
}

uint32_t CCALL FreeSid_c(void *pSid)
{
    free(pSid);
    return 0;
}

uint32_t CCALL InitializeAcl_c(void *pAcl, uint32_t nAclLength, uint32_t dwAclRevision)
{
    (void) pAcl;
    (void) nAclLength;
    (void) dwAclRevision;
    return 1;
}

uint32_t CCALL AddAccessAllowedAce_c(void *pAcl, uint32_t dwAceRevision, uint32_t AccessMask, void *pSid)
{
    (void) pAcl;
    (void) dwAceRevision;
    (void) AccessMask;
    (void) pSid;
    return 1;
}

uint32_t CCALL AddAccessDeniedAce_c(void *pAcl, uint32_t dwAceRevision, uint32_t AccessMask, void *pSid)
{
    (void) pAcl;
    (void) dwAceRevision;
    (void) AccessMask;
    (void) pSid;
    return 1;
}

uint32_t CCALL SetSecurityInfo_c(uint32_t handle, uint32_t ObjectType, uint32_t SecurityInfo, void *psidOwner, void *psidGroup, void *pDacl, void *pSacl)
{
    (void) handle;
    (void) ObjectType;
    (void) SecurityInfo;
    (void) psidOwner;
    (void) psidGroup;
    (void) pDacl;
    (void) pSacl;
    return ERROR_SUCCESS;
}

uint32_t CCALL GetUserNameA_c(void *lpBuffer, void *pcbBuffer)
{
    uint32_t *size = pcbBuffer;

    if (*size < 7)
    {
        *size = 7;
        SRW_SetLastError(122);
        return 0;
    }
    memcpy(lpBuffer, "Player", 7);
    *size = 7;
    return 1;
}
