#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include "cli.h"
#include "connlimit.h"
#include "ppp.h"
#include "events.h"
#include "triton.h"
#include "log.h"
#include "memdebug.h"
#include "utils.h"

struct item
{
	struct list_head entry;
	uint64_t key;
	struct timespec ts;
	int count;
};

static int conf_burst = 3;
static int conf_burst_timeout = 60 * 1000;
static int conf_limit_timeout = 5000;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(items);

int __export connlimit_check(uint64_t key)
{
	struct item *it;
	struct timespec ts;
	unsigned int d;
	struct list_head *pos, *n;
	LIST_HEAD(tmp_list);
	int r = 1;


	clock_gettime(CLOCK_MONOTONIC, &ts);

	pthread_mutex_lock(&lock);
	log_debug("connlimit: check entry %" PRIu64 "\n", key);
	list_for_each_safe(pos, n, &items) {
		it = list_entry(pos, typeof(*it), entry);

		d = (ts.tv_sec - it->ts.tv_sec) * 1000 + (ts.tv_nsec - it->ts.tv_nsec) / 1000000;

		if (it->key == key) {
			if (d >= conf_burst_timeout) {
				it->ts = ts;
				list_move(&it->entry, &items);
				it->count = 0;
				r = 0;
				break;
			}
			it->count++;
			if (it->count >= conf_burst) {
				if (d >= conf_limit_timeout) {
					it->ts = ts;
					list_move(&it->entry, &items);
					r = 0;
				} else
					r = -1;
			} else
				r = 0;
			break;
		}

		if (d > conf_burst_timeout) {
			log_debug("connlimit: remove %" PRIu64 "\n", it->key);
			list_move(&it->entry, &tmp_list);
		}
	}
	pthread_mutex_unlock(&lock);

	if (r == 1) {
		it = _malloc(sizeof(*it));
		memset(it, 0, sizeof(*it));
		it->ts = ts;
		it->key = key;

		log_debug("connlimit: add entry %" PRIu64 "\n", key);

		pthread_mutex_lock(&lock);
		list_add(&it->entry, &items);
		pthread_mutex_unlock(&lock);

		r = 0;
	}

	if (r == 0)
		log_debug("connlimit: accept %" PRIu64 "\n", key);
	else
		log_debug("connlimit: drop %" PRIu64 "\n", key);


	while (!list_empty(&tmp_list)) {
		it = list_entry(tmp_list.next, typeof(*it), entry);
		list_del(&it->entry);
		_free(it);
	}

	return r;
}

static void fmt_age(struct timespec ts, char *buf, char *buf_raw) {
	time_t uptime;
	int day, hour, min, sec;
	char time_str[24];

	uptime = _time() - ts.tv_sec;

	hour = uptime / (60 * 60);
	uptime %= (60 * 60);
	min = uptime / 60;
	sec = uptime % 60;

	snprintf(time_str, sizeof(time_str), "%02i:%02i:%02i", hour, min, sec);

	sprintf(buf, "%s", time_str);
	sprintf(buf_raw,"%lu", (unsigned long)uptime);
}

static void connlimit_show(void *client) {
	struct item *it;
	struct list_head *pos, *n;
	char buf[129], ip[16], age[24], age_raw[12];

	union {
		__u8 mac[ETH_ALEN];
		__u64 mac_u;
	} mac;

	cli_send(client, "       mac         |       ip      | count |    age   | age-raw   \r\n");
	cli_send(client, "-------------------+---------------+-------+----------+-----------\r\n");

	pthread_mutex_lock(&lock);
	list_for_each_safe(pos, n, &items) {
 		it = list_entry(pos, typeof(*it), entry);
		if (it->key > UINT32_MAX) {
			mac.mac_u = it->key;
			sprintf(buf, " %02x:%02x:%02x:%02x:%02x:%02x | %-12s ", mac.mac[0], mac.mac[1], mac.mac[2], mac.mac[3], mac.mac[4], mac.mac[5], " ");
		} else {
			u_inet_ntoa(it->key, ip);
			sprintf(buf, " %-18s| %-13s", " ", ip);
		}
		fmt_age(it->ts, age, age_raw);
		cli_sendv(client, "%s | %-5d | %-8s | %s \r\n", buf, it->count, age, age_raw);
	}

	pthread_mutex_unlock(&lock);
}

static void connlimit_flush(void) {
	struct item *it;
	pthread_mutex_lock(&lock);
	while (!list_empty(&items)) {
		it = list_entry(items.next, typeof(*it), entry);
		list_del(&it->entry);
		_free(it);
	}
	pthread_mutex_unlock(&lock);
	log_debug("connlimit: remove all entries\n");
}

static void connlimit_flush_key(uint64_t key) {
	struct item *it;
	struct list_head *pos, *n;

	pthread_mutex_lock(&lock);

	list_for_each_safe(pos, n, &items) {
		it = list_entry(pos, typeof(*it), entry);
		if (it->key == key) {
			log_debug("connlimit: remove %" PRIu64 "\n", it->key);
			list_del(&it->entry);
			_free(it);
			break;
		}
	}
	pthread_mutex_unlock(&lock);
}

static void connlimit_flush_mac(const char *addr, void *client) {
	uint8_t a[ETH_ALEN];

	if (sscanf(addr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
		cli_send(client, "invalid format\r\n");
		return;
	}
	connlimit_flush_key(cl_key_from_mac(a));
}

static void connlimit_flush_ip(const char *addr, void *client) {
	struct in_addr ip_addr;
	size_t len;
	len = u_parse_ip4addr(addr, &ip_addr);
	if (!len) {
		cli_send(client, "invalid format\r\n");
		return;
	}
	connlimit_flush_key(cl_key_from_ipv4(ip_addr.s_addr));
}

static void cmd_help(char * const *fields, int fields_cnt, void *client)
{
	cli_send(client, "connlimit show - show connection limit entries\r\n");
	cli_send(client, "connlimit flush - flush connection limit entries\r\n");
	cli_send(client, "\tip <addresss> - flush by ip address\r\n");
	cli_send(client, "\tmac <mac> - flush by station mac address\r\n");
	cli_send(client, "\tall - flush all entries\r\n");
}

static int cmd_exec(const char *cmd, char * const *fields, int fields_cnt, void *client) {
	if (fields_cnt < 2)
		goto help;
	if (!strcmp(fields[1], "flush")) {
		if (fields_cnt == 3 && !strcmp(fields[2], "all")) {
			connlimit_flush();
		} else if (fields_cnt == 4 && !strcmp(fields[2], "mac")) {
			connlimit_flush_mac(fields[3], client);
		} else if (fields_cnt == 4 && !strcmp(fields[2], "ip")) {
			connlimit_flush_ip(fields[3], client);
		} else
			goto help;
	} else if (fields_cnt == 2 && !strcmp(fields[1], "show")) {
		connlimit_show(client);
	} else
		goto help;

	return CLI_CMD_OK;
	help:
	cmd_help(fields, fields_cnt, client);
	return CLI_CMD_OK;
}

static int parse_limit(const char *opt, int *limit, int *time)
{
	char *endptr;

	*limit = strtol(opt, &endptr, 10);

	if (!*endptr) {
		*time = 1;
		return 0;
	}

	if (*endptr != '/')
		goto out_err;

	opt = endptr + 1;
	*time = strtol(opt, &endptr, 10);

	if (endptr == opt)
		*time = 1;

	if (*endptr == 's')
		return 0;

	if (*endptr == 'm') {
		*time *= 60;
		return 0;
	}

	if (*endptr == 'h') {
		*time *= 3600;
		return 0;
	}

out_err:
	log_error("connlimit: failed to parse '%s'\n", opt);
	return -1;
}

static void load_config()
{
	const char *opt;
	int n,t;

	opt = conf_get_opt("connlimit", "limit");
	if (opt) {
		if (parse_limit(opt, &n, &t))
			return;
		conf_limit_timeout = t * 1000 / n;
	}

	opt = conf_get_opt("connlimit", "burst");
	if (opt)
		conf_burst = atoi(opt);

	opt = conf_get_opt("connlimit", "timeout");
	if (opt)
		conf_burst_timeout = atoi(opt) * 1000;
}


static void init()
{
	load_config();

	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
	cli_register_simple_cmd2(cmd_exec, cmd_help, 1, "connlimit");
}

DEFINE_INIT(200, init);
