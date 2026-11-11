#pragma once
#include <stdbool.h>
#include "cJSON.h"

void uart_proto_init(void);
/* Emit {"event":name,"data":data} line. Takes ownership of data (may be NULL). */
void uart_proto_event(const char *name, cJSON *data);
/* '#'-prefixed log line, only when log enabled via sys.log */
void uart_proto_log(const char *level, const char *fmt, ...);
void uart_proto_set_log_level(int level); /* 0 none,1 err,2 warn,3 info,4 debug */
