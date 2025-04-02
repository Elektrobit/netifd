#include <string.h>
#include <sys/wait.h>

#include "netifd.h"
#include "proto.h"
#include "interface.h"
#include "interface-ip.h"
#include "device.h"
#include "system.h"

struct dhcp_proto_state {
	struct interface_proto_state proto;

	struct blob_attr *config;

	bool dhcpv6;
	bool teardown;

	enum {
		DHCP_IDLE,
		DHCP_SETTING_UP,
		DHCP_DONE
	} state;

	struct netifd_process client;
};

enum {
	NOTIFY_REASON,
	NOTIFY_IP,
	NOTIFY_SUBNET,
	NOTIFY_ROUTER,
	NOTIFY_DNS,
	NOTIFY_MTU,
	NOTIFY_IP6,
	NOTIFY_IP6_VALID,
	__NOTIFY_LAST
};

static const struct blobmsg_policy notify_attr[__NOTIFY_LAST] = {
	[NOTIFY_REASON] = { .name = "reason", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_IP] = { .name = "ip", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_SUBNET] = { .name = "subnet", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_ROUTER] = { .name = "router", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_DNS] = { .name = "dns", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_MTU] = { .name = "mtu", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_IP6] = { .name = "ipv6", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_IP6_VALID] = { .name = "lease", .type = BLOBMSG_TYPE_STRING },
};

static int udhcp_proto_setup(struct dhcp_proto_state *state);

static void
dhcp_process_callback(struct netifd_process *proc, int ret)
{
	struct dhcp_proto_state *state = container_of(proc, struct dhcp_proto_state, client);
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
		netifd_log_message(L_WARNING, "Dhcp client terminated unexpectedly with %s %d\n", desc, code);
		udhcp_proto_setup(state);
	}
}

static void
start_dhcp_client(struct dhcp_proto_state *state)
{
	const char *argv[] = {
		state->dhcpv6 ? "/usr/bin/udhcpc6" : "/sbin/udhcpc",
		"-i", state->proto.iface->main_dev.dev->ifname,
		"-s", "/usr/libexec/netifd/udhcp-script",
		"-R",
		"-f",
		"-t", "0",
		"-O", "mtu",
		NULL
	};

	if (state->dhcpv6) {
		/* Select dns instead of mtu for dhcp6 */
		argv[10] = "dns";
	}

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
udhcp_proto_setup(struct dhcp_proto_state *state)
{
	struct interface *iface = state->proto.iface;
	struct device *dev = state->proto.iface->main_dev.dev;
	interface_set_l3_dev(iface, dev);

	state->state = DHCP_SETTING_UP;

	state->teardown = false;
	start_dhcp_client(state);

	return 0;
}

static int
udhcp_handler(struct interface_proto_state *proto,
	       enum interface_proto_cmd cmd, bool force)
{
	int ret = 0;

	struct dhcp_proto_state *state = container_of(proto, struct dhcp_proto_state, proto);

	switch (cmd) {
	case PROTO_CMD_SETUP:
		ret = udhcp_proto_setup(state);
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
		if (state->client.uloop.pending) {
			kill(state->client.uloop.pid, SIGUSR1);
		}
		break;
	}

	return ret;
}

static void
udhcp_set_dns_servers(struct interface *iface, struct blob_attr *attr)
{
	if (attr == NULL)
		return;

	struct blob_buf b = {};
	blobmsg_buf_init(&b);

	char *dnss = blobmsg_get_string(attr);
	char *saveptr = NULL;
	for (int i = 0; ; i++) {
		char *dns = strtok_r(i == 0 ? dnss : NULL, " ", &saveptr);
		if (dns == NULL)
			break;

		blobmsg_add_string(&b, "dns", dns);
	}
	interface_add_dns_server_list(&iface->proto_ip, b.head);
	blob_buf_free(&b);
}

static int
udhcp4_configure(struct dhcp_proto_state *state, const char *action, struct blob_attr **tb)
{
	struct interface *iface = state->proto.iface;
	if (strcmp(action, "deconfig") == 0) {
		interface_update_start(iface, false);
		interface_update_complete(iface);
		state->proto.proto_event(&state->proto, IFPEV_LINK_LOST);
	} else if (strcmp(action, "bound") == 0 || strcmp(action, "renew") == 0) {
		interface_update_start(iface, false);

		struct blob_buf b = {};
		blobmsg_buf_init(&b);
		void *cookie = blobmsg_open_array(&b, "ipaddr");
		void *cookie2 = blobmsg_open_table(&b, "ipaddr");
		if (tb[NOTIFY_IP])
			blobmsg_add_string(&b, "ipaddr", blobmsg_get_string(tb[NOTIFY_IP]));
		if (tb[NOTIFY_SUBNET])
			blobmsg_add_string(&b, "mask", blobmsg_get_string(tb[NOTIFY_SUBNET]));
		blobmsg_close_table(&b, cookie2);
		blobmsg_close_array(&b, cookie);
		proto_apply_ip_settings(iface, b.head, false);
		blob_buf_free(&b);

		if (tb[NOTIFY_ROUTER]) {
			char *routers = blobmsg_get_string(tb[NOTIFY_ROUTER]);
			char *saveptr = NULL;
			for (int i = 0; ; i++) {
				char *router = strtok_r(i == 0 ? routers : NULL, " ", &saveptr);
				if (router == NULL) break;

				blobmsg_buf_init(&b);
				blobmsg_add_string(&b, "target", "0.0.0.0");
				blobmsg_add_string(&b, "netmask", "0.0.0.0");
				blobmsg_add_string(&b, "gateway", router);
				blobmsg_add_string(&b, "source", blobmsg_get_string(tb[NOTIFY_IP]));
				blobmsg_add_u32(&b, "metric", i + 1);

				interface_ip_add_route(iface, b.head, false);
				blob_buf_free(&b);
			}
		}

		if (tb[NOTIFY_DNS]) {
			udhcp_set_dns_servers(iface, tb[NOTIFY_DNS]);
		}

		if (tb[NOTIFY_MTU]) {
			char * end;
			const char *smtu = blobmsg_get_string(tb[NOTIFY_MTU]);
			int mtu = strtol(smtu, &end, 10);
			if (end[0] == '\0' && mtu > 58 && mtu <= 65535) {
				iface->l3_dev.dev->settings.mtu = mtu;
				iface->l3_dev.dev->settings.flags |= DEV_OPT_MTU;
				system_if_apply_settings(iface->l3_dev.dev, &iface->l3_dev.dev->settings, DEV_OPT_MTU);
			}
		}

		interface_update_complete(iface);

		state->state = DHCP_DONE;
		state->proto.proto_event(&state->proto, IFPEV_UP);
	}
	return 0;
}

static int
udhcp6_configure(struct dhcp_proto_state *state, const char *action, struct blob_attr **tb)
{
	struct interface *iface = state->proto.iface;
	if (strcmp(action, "deconfig") == 0) {
		interface_update_start(iface, false);
		interface_update_complete(iface);
		if (state->teardown) {
			state->proto.proto_event(&state->proto, IFPEV_DOWN);
		} else {
			state->proto.proto_event(&state->proto, IFPEV_LINK_LOST);
		}
	} else if (strcmp(action, "bound") == 0 || strcmp(action, "renew") == 0) {
		interface_update_start(iface, false);

		struct blob_buf b = {};
		blobmsg_buf_init(&b);
		void *cookie = blobmsg_open_array(&b, "ip6addr");
		void *cookie2 = blobmsg_open_table(&b, "ip6addr");
		if (tb[NOTIFY_IP6])
			blobmsg_add_string(&b, "ipaddr", blobmsg_get_string(tb[NOTIFY_IP6]));
		if (tb[NOTIFY_IP6_VALID]) {
			char *end;
			int valid = strtol(blobmsg_get_string(tb[NOTIFY_IP6_VALID]), &end, 10);
			if (end && *end == '\0') {
				blobmsg_add_u32(&b, "valid", valid);
			}
		}
		blobmsg_close_table(&b, cookie2);
		blobmsg_close_array(&b, cookie);
		proto_apply_ip_settings(iface, b.head, false);
		blob_buf_free(&b);

		if (tb[NOTIFY_DNS]) {
			udhcp_set_dns_servers(iface, tb[NOTIFY_DNS]);
		}

		interface_update_complete(iface);

		state->state = DHCP_DONE;
		state->proto.proto_event(&state->proto, IFPEV_UP);
	}
	return 0;
}

static int
udhcp_notify(struct interface_proto_state *proto, struct blob_attr *attr)
{
	struct dhcp_proto_state *state = container_of(proto, struct dhcp_proto_state, proto);

	struct blob_attr *tb[__NOTIFY_LAST];
	blobmsg_parse(notify_attr, __NOTIFY_LAST, tb, blob_data(attr), blob_len(attr));

	if (!tb[NOTIFY_REASON]) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	char *action = blobmsg_get_string(tb[NOTIFY_REASON]);

	if (state->dhcpv6) {
		return udhcp6_configure(state, action, tb);
	} else {
		return udhcp4_configure(state, action, tb);
	}
}

static void
udhcp_free(struct interface_proto_state *proto)
{
	struct dhcp_proto_state *state;

	state = container_of(proto, struct dhcp_proto_state, proto);
	free(state);
}

static struct proto_handler udhcp_proto6;

static struct interface_proto_state *
udhcp_attach(const struct proto_handler *h, struct interface *iface,
	      struct blob_attr *attr)
{
	struct dhcp_proto_state *state;

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->config = malloc(blob_pad_len(attr));
	if (!state->config) {
		free(state);
		return NULL;
	}
	memcpy(state->config, attr, blob_pad_len(attr));

	state->proto.free = udhcp_free;
	state->proto.cb = udhcp_handler;
	state->proto.notify = udhcp_notify;

	state->dhcpv6 = h == &udhcp_proto6;
	state->teardown = false;

	state->client.cb = dhcp_process_callback;
	state->client.log_prefix = iface->name;
	state->state = DHCP_IDLE;

	return &state->proto;
}

static struct proto_handler udhcp_proto = {
	.name = "udhcp",
	.flags = PROTO_FLAG_RENEW_AVAILABLE,
	.attach = udhcp_attach,
};

static struct proto_handler udhcp_proto6 = {
	.name = "udhcp6",
	.flags = PROTO_FLAG_RENEW_AVAILABLE,
	.attach = udhcp_attach,
};

static void __init
isc_dhcp_proto_init(void)
{
	add_proto_handler(&udhcp_proto);
	add_proto_handler(&udhcp_proto6);
}
