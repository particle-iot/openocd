#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "trace_server.h"

#define TRACE_SERVER_SERVICE_NAME "trace server"

struct trce_server_connection {
	int tc_outerror;/* flag an output error */
};

static const char *trace_server_port;

static struct service *trace_server;

/* handlers */
static int trace_server_new_connection(struct connection *connection);
static int trace_server_input(struct connection *connection);
static int trace_server_output(struct connection *connection, const void *buf, ssize_t len);
static int trace_server_closed(struct connection *connection);

/* write data out to a socket.
 *
 * this is a blocking write, so the return value must equal the length, if
 * that is not the case then flag the connection with an output error.
 */
int trace_server_output(struct connection *connection, const void *data, ssize_t len)
{
	ssize_t wlen;
	struct trce_server_connection *tclc;

	tclc = connection->priv;
	if (tclc->tc_outerror)
		return ERROR_SERVER_REMOTE_CLOSED;

	wlen = connection_write(connection, data, len);

	if (wlen == len)
		return ERROR_OK;

	LOG_ERROR("error during write: %d != %d", (int)wlen, (int)len);
	tclc->tc_outerror = 1;
	return ERROR_SERVER_REMOTE_CLOSED;
}

/* connections */
static int trace_server_new_connection(struct connection *connection)
{
	struct trce_server_connection *tclc;

	tclc = malloc(sizeof(struct trce_server_connection));
	if (tclc == NULL)
		return ERROR_CONNECTION_REJECTED;

	memset(tclc, 0, sizeof(struct trce_server_connection));
	connection->priv = tclc;
	return ERROR_OK;
}

static int trace_server_input(struct connection *connection)
{
	ssize_t rlen;
	unsigned char in[256];

	rlen = connection_read(connection, &in, sizeof(in));
	if (rlen <= 0) {
		if (rlen < 0)
			LOG_ERROR("error during read: %s", strerror(errno));
		return ERROR_SERVER_REMOTE_CLOSED;
	}

	LOG_WARNING("trace server input not supported");

	return ERROR_OK;
}

static int trace_server_closed(struct connection *connection)
{
	/* cleanup connection context */
	if (connection->priv) {
		free(connection->priv);
		connection->priv = NULL;
	}
	return ERROR_OK;
}

int trace_server_init(void)
{
	trace_server = NULL;
	if (strcmp(trace_server_port, "disabled") == 0) {
		LOG_INFO("trace server disabled");
		return ERROR_OK;
	}
	int rez = add_service(TRACE_SERVER_SERVICE_NAME, trace_server_port, 4,
		&trace_server_new_connection, &trace_server_input,
		&trace_server_closed, NULL);
	trace_server = find_by_name(TRACE_SERVER_SERVICE_NAME);
	return rez;
}

COMMAND_HANDLER(handle_trace_server_port_command)
{
	return CALL_COMMAND_HANDLER(server_pipe_command, &trace_server_port);
}

static const struct command_registration trace_server_command_handlers[] = {
	{
		.name = "trace_port",
		.handler = handle_trace_server_port_command,
		.mode = COMMAND_CONFIG,
		.help = "Specify port on which to listen "
			"for Trace server.  "
			"Read help on 'trace_port'.",
		.usage = "[port_num]",
	},
	COMMAND_REGISTRATION_DONE
};

int trace_server_register_commands(struct command_context *cmd_ctx)
{
	trace_server_port = strdup("7777");
	return register_commands(cmd_ctx, NULL, trace_server_command_handlers);
}


int trace_server_output_all(const void *data, ssize_t len)
{
	struct connection *t_conn = trace_server->connections;
	while (t_conn != NULL) {
		trace_server_output(t_conn, data, len);
		t_conn = t_conn->next;
	}
	return ERROR_OK;
}
