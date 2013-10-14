#ifndef TRACE_SERVER_H
#define TRACE_SERVER_H

#include <server/server.h>

int trace_server_init(void);
int trace_server_register_commands(struct command_context *cmd_ctx);

int trace_server_output_all(const void *data, ssize_t len);


#endif	/* TRACE_SERVER_H */
