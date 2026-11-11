#pragma once
#include <stdbool.h>
#include "cJSON.h"

/* error codes (see docs/DESIGN.md §5.3) */
#define E_PARSE          -32700
#define E_INVALID_REQ    -32600
#define E_NO_METHOD      -32601
#define E_INVALID_PARAMS -32602
#define E_INTERNAL       -32603
#define E_BLE_NOT_CONN   1001
#define E_HID_BUSY       1002
#define E_TIMEOUT        1003
#define E_WIFI_NOT_CONN  2001
#define E_WIFI_AUTH      2002
#define E_CFG_WRITE      3001
#define W_REBOOT_REQ     3002
#define E_UNAUTHORIZED   4001

typedef enum { RPC_SRC_UART, RPC_SRC_HTTP, RPC_SRC_MCP } rpc_src_t;

typedef struct {
    int code;              /* 0 = ok */
    char msg[96];
    bool reboot_required;  /* adds warn 3002 to response */
} rpc_err_t;

void rpc_set_err(rpc_err_t *e, int code, const char *fmt, ...);

/* Execute one method. Returns result object (caller frees) or NULL on error.
 * req_id is used for async completion events (hid.done). */
cJSON *rpc_call(const char *method, const cJSON *params, rpc_src_t src,
                int req_id, rpc_err_t *err);

/* MCP tool catalogue */
typedef struct {
    const char *method;       /* e.g. "hid.click"; tool name = '.'→'_' */
    const char *description;
    const char *input_schema; /* JSON schema text */
} rpc_tool_t;
const rpc_tool_t *rpc_tools(int *count);
