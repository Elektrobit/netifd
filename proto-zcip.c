#include <string.h>
#include <sys/wait.h>

#include "netifd.h"
#include "proto.h"
#include "interface.h"
#include "interface-ip.h"
#include "device.h"
#include "system.h"

struct zcip_proto_state {
	struct interface_proto_state proto;

	struct blob_attr *config;

	bool teardown;

	enum {
		ZCIP_IDLE,
		ZCIP_SETTING_UP,
		ZCIP_DONE
	} state;

	struct netifd_process client;
};

enum {
	NOTIFY_REASON,
	NOTIFY_IP,
	__NOTIFY_LAST
};

static const struct blobmsg_policy notify_attr[__NOTIFY_LAST] = {
	[NOTIFY_REASON] = { .name = "reason", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_IP] = { .name = "ip", .type = BLOBMSG_TYPE_STRING },
};

static int zcip_proto_setup(struct zcip_proto_state *state);

static void
zcip_process_callback(struct netifd_process *proc, int ret)
{
	struct zcip_proto_state *state = container_of(proc, struct zcip_proto_state, client);
	if (state->teardown) {
		state->proto.proto_event(&state->proto, IFPEV_DOWN);
	} else { /* unexpected shutdown, restart */
		int code = -1;
		char *desc = "unknown";
		if (WIFEXITED(ret)) {
			desc = "exit code";
			code = WEXITSTATUS(ret);
		} else if (WIFSIGNALED(ret)) {
			desc = "signal";
			code = WTERMSIG(ret);
		}
		netifd_log_message(L_WARNING, "zcip terminated unexpectedly with %s %d\n", desc, code);
		zcip_proto_setup(state);
	}
}

static void
start_zcip_client(struct zcip_proto_state *state)
{
	const char *argv[] = {
		"/usr/sbin/zcip",
		"-f",
        state->proto.iface->main_dev.dev->ifname,
        "/usr/libexec/netifd/zcip-script",
		NULL
	};

	/* inject netifd interface name into environment for notify*/
	char iface[256];
	snprintf(iface, sizeof(iface), "NETIFD_INTERFACE=%s", state->proto.iface->name);
	char *env[] = {
		iface,
		NULL
	};

	netifd_start_process(argv, env, &state->client);
}

static int
zcip_proto_setup(struct zcip_proto_state *state)
{
	struct interface *iface = state->proto.iface;
	struct device *dev = state->proto.iface->main_dev.dev;
	interface_set_l3_dev(iface, dev);

	state->state = ZCIP_SETTING_UP;

	state->teardown = false;
	start_zcip_client(state);

	return 0;
}

static int
zcip_handler(struct interface_proto_state *proto,
	       enum interface_proto_cmd cmd, bool force)
{
	int ret = 0;

	struct zcip_proto_state *state = container_of(proto, struct zcip_proto_state, proto);

	switch (cmd) {
	case PROTO_CMD_SETUP:
		ret = zcip_proto_setup(state);
		break;
	case PROTO_CMD_TEARDOWN:
		if (state->client.uloop.pending) {
			state->teardown = true;
			kill(state->client.uloop.pid, SIGTERM);
		} else {
			state->proto.proto_event(&state->proto, IFPEV_DOWN);
		}
		break;
	case PROTO_CMD_RENEW:
        return 1; // Not implemented
	}

	return ret;
}

static int
zcip_configure(struct zcip_proto_state *state, const char *action, struct blob_attr **tb)
{
	struct interface *iface = state->proto.iface;
    if (strcmp(action, "init") == 0) {
        // nothing to do
	} else if (strcmp(action, "deconfig") == 0) {
		interface_update_start(iface, false);
		interface_update_complete(iface);
		state->proto.proto_event(&state->proto, IFPEV_LINK_LOST);
	} else if (strcmp(action, "config") == 0) {
		interface_update_start(iface, false);

		struct blob_buf b = {};
		blobmsg_buf_init(&b);
		void *cookie = blobmsg_open_array(&b, "ipaddr");
		void *cookie2 = blobmsg_open_table(&b, "ipaddr");
		if (tb[NOTIFY_IP])
			blobmsg_add_string(&b, "ipaddr", blobmsg_get_string(tb[NOTIFY_IP]));
        blobmsg_add_string(&b, "mask", "16");
        blobmsg_add_string(&b, "scope", "link");
		blobmsg_close_table(&b, cookie2);
		blobmsg_close_array(&b, cookie);
		proto_apply_ip_settings(iface, b.head, false);
		blob_buf_free(&b);

		interface_update_complete(iface);

		state->state = ZCIP_DONE;
		state->proto.proto_event(&state->proto, IFPEV_UP);
	}
	return 0;
}

static int
zcip_notify(struct interface_proto_state *proto, struct blob_attr *attr)
{
	struct zcip_proto_state *state = container_of(proto, struct zcip_proto_state, proto);

	struct blob_attr *tb[__NOTIFY_LAST];
	blobmsg_parse(notify_attr, __NOTIFY_LAST, tb, blob_data(attr), blob_len(attr));

	if (!tb[NOTIFY_REASON]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	char *action = blobmsg_get_string(tb[NOTIFY_REASON]);

	return zcip_configure(state, action, tb);
}

static void
zcip_free(struct interface_proto_state *proto)
{
	struct zcip_proto_state *state;

	state = container_of(proto, struct zcip_proto_state, proto);
	free(state);
}

static struct interface_proto_state *
zcip_attach(const struct proto_handler *h, struct interface *iface,
	      struct blob_attr *attr)
{
	struct zcip_proto_state *state;

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->config = malloc(blob_pad_len(attr));
	if (!state->config) {
		free(state);
		return NULL;
	}
	memcpy(state->config, attr, blob_pad_len(attr));

	state->proto.free = zcip_free;
	state->proto.cb = zcip_handler;
	state->proto.notify = zcip_notify;

	state->teardown = false;

	state->client.cb = zcip_process_callback;
	state->client.log_prefix = iface->name;
	state->state = ZCIP_IDLE;

	return &state->proto;
}

static struct proto_handler zcip_proto = {
	.name = "zcip",
	.flags = 0,
	.attach = zcip_attach,
};

static void __init
isc_zcip_proto_init(void)
{
	add_proto_handler(&zcip_proto);
}
