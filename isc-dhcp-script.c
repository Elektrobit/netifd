#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <net/if.h>
#include <linux/if_addr.h>


#include <sys/socket.h>

#include <netlink/msg.h>
#include <netlink/attr.h>
#include <netlink/socket.h>
#include <netlink/handlers.h>
#include <linux/rtnetlink.h>

struct cb_data {
    unsigned int index;
    bool permanent;
};

static volatile bool gTimeout = false;

static void
alarm_hdl(int sig)
{
    gTimeout = true;
}

static int
nl_callback(struct nl_msg *msg, void *arg)
{
    struct cb_data *data = arg;
    struct nlmsghdr *hdr = nlmsg_hdr(msg);

    if (hdr->nlmsg_type == RTM_NEWADDR) {
        struct ifaddrmsg *ifa = NLMSG_DATA(hdr);
        if (ifa->ifa_index == data->index && ifa->ifa_scope == RT_SCOPE_LINK && !(ifa->ifa_flags & IFA_F_TENTATIVE)) {
            data->permanent = true;
            return NL_STOP;
        }
    }
    return NL_OK;
}

static struct nl_sock *
create_socket(void)
{
	struct nl_sock *sock;

	sock = nl_socket_alloc();
	if (!sock)
		return NULL;

	if (nl_connect(sock, NETLINK_ROUTE)) {
		nl_socket_free(sock);
		return NULL;
	}

	return sock;
}

static bool
interface_has_permanent_local_address(struct nl_sock *sock, int ifindex)
{
    struct nl_msg *msg = nlmsg_alloc_simple(RTM_GETADDR, NLM_F_REQUEST | NLM_F_DUMP);
    struct ifaddrmsg rt = { .ifa_family = AF_INET6 };

    nlmsg_append(msg, &rt, sizeof(rt), 0);
    nl_send_auto_complete(sock, msg);
    nlmsg_free(msg);


    struct nl_cb *cb = nl_cb_alloc(NL_CB_DEFAULT);
    struct cb_data data = {
        .index = ifindex,
        .permanent = false
    };

    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, nl_callback, &data);
    nl_recvmsgs(sock, cb);

    nl_cb_put(cb);

    return data.permanent;
}

static void
wait_for_dad(const char *interface, int timeout)
{
    int ifindex = if_nametoindex(interface);
    if (ifindex == 0 || timeout <= 0)
        return;

    struct nl_sock *sock = create_socket();

    signal(SIGALRM, alarm_hdl);
    alarm(timeout);

    while (!gTimeout && !interface_has_permanent_local_address(sock, ifindex)) {
        usleep(100000);
    }

    nl_close(sock);
}

int
main(int argc, char **argv)
{
    if (argc != 1 || !getenv("NETIFD_INTERFACE")) {
        fprintf(stderr, "This program should only be called by isc dhcpc\n");
        return -1;
    }

    struct blob_buf b = {};

    blobmsg_buf_init(&b);
    char **cur = __environ;
    char key[128];
    for (;*cur; cur++) {
        char *end = strchr(*cur, '=');
        if (!end || end - *cur >= (int) sizeof(key))
            continue; // Ignore environment entries with very long or invalid keys
        memcpy(key, *cur, end - *cur);
        key[end - *cur] = 0;
        blobmsg_add_string(&b, key, end + 1);
    }

    struct ubus_context *ctx = ubus_connect(NULL);
	if (!ctx) {
		fprintf(stderr, "Failed to connect to ubus\n");
		return -1;
	}

    uint32_t id;
    char name[256];
    snprintf(name, sizeof(name), "network.interface.%s", getenv("NETIFD_INTERFACE"));
    int ret = ubus_lookup_id(ctx, name, &id);
	if (ret)
		return ret;

	ret = ubus_invoke(ctx, id, "notify_proto", b.head, NULL, NULL, 1000);
    if (ret != 0) {
        fprintf(stderr, "Ubus call failed: %s\n", ubus_strerror(ret));
    }

    const char *reason = getenv("reason");
    if (reason && strcmp(reason, "PREINIT6") == 0) {
        const char *interface = getenv("interface");

        if (interface) {
            const char *dad_wait_string = getenv("dad_wait_time");
            int timeout = 5;
            if (dad_wait_string) {
                char *end;
                timeout = strtol(dad_wait_string, &end, 10);
                if (!end || end[0] != '\0')
                    timeout = 5;
            }
            wait_for_dad(interface, timeout);
        }
    }

    return 0;
}

