#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>

#include <libubus.h>
#include <libubox/blobmsg.h>

int
main(int argc, char **argv)
{
    if (argc != 2 || !getenv("NETIFD_INTERFACE")) {
        fprintf(stderr, "This program should only be called by udhcpc\n");
        return -1;
    }
    const char *reason = argv[1];

    struct blob_buf b = {};

    blobmsg_buf_init(&b);
    char **cur = __environ;
    char key[128];
    for (;*cur; cur++) {
        char *end = strchr(*cur, '=');
        if (!end || end - *cur >= (long int)sizeof(key))
            continue; // Ignore environment entries with very long or invalid keys
        memcpy(key, *cur, end - *cur);
        key[end - *cur] = 0;
        blobmsg_add_string(&b, key, end + 1);
    }
    blobmsg_add_string(&b, "reason", reason);

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

    return 0;
}

