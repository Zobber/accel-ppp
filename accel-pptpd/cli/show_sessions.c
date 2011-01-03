#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>

#include "triton.h"
#include "events.h"
#include "ppp.h"
#include "cli.h"
#include "utils.h"
#include "log.h"
#include "memdebug.h"

#define CELL_SIZE 128
#define DEF_COLUMNS "ifname,username,calling-sid,ip,rate-limit,type,state,uptime"

struct column_t
{
	struct list_head entry;
	const char *name;
	const char *desc;
	void (*print)(const struct ppp_t *ppp, char *buf);
};

struct col_t
{
	struct list_head entry;
	struct column_t *column;
	int width;
};

struct row_t
{
	struct list_head entry;
	char *match_key;
	char *order_key;
	struct list_head cell_list;
};

struct cell_t
{
	struct list_head entry;
	struct col_t *col;
	char buf[CELL_SIZE + 1];
};

static LIST_HEAD(col_list);

void __export cli_show_ses_register(const char *name, const char *desc, void (*print)(const struct ppp_t *ppp, char *buf))
{
	struct column_t *c = malloc(sizeof(*c));
	c->name = name;
	c->desc = desc;
	c->print = print;
	list_add_tail(&c->entry, &col_list);
}

static void show_ses_help(char * const *f, int f_cnt, void *cli)
{
	struct column_t *col;
	char buf[129];

	cli_send(cli, "show sessions [columns] [order <column>] [match <column> <regexp>] - shows sessions\r\n");
	cli_send(cli, "\tcolumns:\r\n");

	list_for_each_entry(col, &col_list, entry) {
		snprintf(buf, 128, "\t\t%s - %s\r\n", col->name, col->desc);
		cli_send(cli, buf);
	}
}

static struct column_t *find_column(const char *name)
{
	struct column_t *col;

	list_for_each_entry(col, &col_list, entry) {
		if (strcmp(col->name, name))
			continue;
		return col;
	}

	return NULL;
}

static void free_row(struct row_t *row)
{
	struct cell_t *cell;

	while (!list_empty(&row->cell_list)) {
		cell = list_entry(row->cell_list.next, typeof(*cell), entry);
		list_del(&cell->entry);
		_free(cell);
	}

	_free(row);
}

static void insert_row(struct list_head *list, struct row_t *row)
{
	struct row_t *row2, *row3;
				
	row3 = NULL;
	list_for_each_entry(row2, list, entry) {
		if (strcmp(row->order_key, row2->order_key) <= 0) {
			row3 = row2;
			break;
		}
	}
	if (row3)
		list_add_tail(&row->entry, &row3->entry);
	else
		list_add_tail(&row->entry, list);
}

static int show_ses_exec(const char *cmd, char * const *f, int f_cnt, void *cli)
{
	char *columns = NULL;
	struct column_t *match_key = NULL;
	char *match_pattern = NULL;
	struct column_t *order_key = NULL;
	pcre *re = NULL;
	const char *pcre_err;
	int pcre_offset;
	struct column_t *column;
	struct col_t *col;
	struct row_t *row;
	struct cell_t *cell;
	char *ptr1, *ptr2;
	int i, n, total_width, def_columns = 0;
	struct ppp_t *ppp;
	char *buf = NULL;
	LIST_HEAD(c_list);
	LIST_HEAD(r_list);
	LIST_HEAD(t_list);

	for (i = 2; i < f_cnt; i++) {
		if (!strcmp(f[i], "order")) {
			if (i == f_cnt - 1)
				return CLI_CMD_SYNTAX;
			order_key = find_column(f[++i]);
			if (!order_key) {
				cli_sendv(cli, "unknown column %s\r\n", f[i]);
				return CLI_CMD_OK;
			}
		} else if (!strcmp(f[i], "match")) {
			if (i == f_cnt - 2)
				return CLI_CMD_SYNTAX;
			match_key = find_column(f[++i]);
			if (!match_key) {
				cli_sendv(cli, "unknown column %s\r\n", f[i]);
				return CLI_CMD_OK;
			}
			match_pattern = f[++i];
		} else if (!columns)
			columns = f[i];
		else
			return CLI_CMD_SYNTAX;
	}

	if (match_key) {
		re = pcre_compile2(match_pattern, 0, NULL, &pcre_err, &pcre_offset, NULL);
		if (!re) {
			cli_sendv(cli, "match: %s at %i\r\n", pcre_err, pcre_offset);
			return CLI_CMD_OK;
		}
	}

	if (!columns) {
		columns = DEF_COLUMNS;
		def_columns = 1;
	}
	
	columns = _strdup(columns);
	ptr1 = columns;
	while (1) {
		ptr2 = strchr(ptr1, ',');
		if (ptr2)
			*ptr2 = 0;
		column = find_column(ptr1);
		if (column) {
			col = _malloc(sizeof(*col));
			col->column = column;
			col->width = strlen(column->name);
			list_add_tail(&col->entry, &c_list);
		} else {
			if (!def_columns) {
				cli_sendv(cli, "unknown column %s\r\n", ptr1);
				_free(columns);
				goto out;
			}
		}
		if (!ptr2)
			break;
		ptr1 = ptr2 + 1;
	}
	_free(columns);

	pthread_rwlock_rdlock(&ppp_lock);
	list_for_each_entry(ppp, &ppp_list, entry) {
		row = _malloc(sizeof(*row));
		if (!row)
			goto oom;
		memset(row, 0, sizeof(*row));
		INIT_LIST_HEAD(&row->cell_list);
		if (match_key || order_key)
			list_add_tail(&row->entry, &t_list);
		else
			list_add_tail(&row->entry, &r_list);
		list_for_each_entry(col, &c_list, entry) {
			cell = _malloc(sizeof(*cell));
			if (!cell)
				goto oom;
			cell->col = col;
			list_add_tail(&cell->entry, &row->cell_list);
			col->column->print(ppp, cell->buf);
			n = strlen(cell->buf);
			if (n > col->width)
				col->width = n;
			if (col->column == order_key)
				row->order_key = cell->buf;
			if (col->column == match_key)
				row->match_key = cell->buf;
		}
	}
	pthread_rwlock_unlock(&ppp_lock);

	if (order_key || match_key) {
		while(!list_empty(&t_list)) {
			row = list_entry(t_list.next, typeof(*row), entry);
			list_del(&row->entry);
			if (match_key) {
				if (pcre_exec(re, NULL, row->match_key, strlen(row->match_key), 0, 0, NULL, 0) < 0) {
					free_row(row);
					continue;
				}
			}
			if (order_key)
				insert_row(&r_list, row);
			else
				list_add_tail(&row->entry, &r_list);
		}
	}

	total_width = -1;
	list_for_each_entry(col, &c_list, entry)
		total_width += col->width + 3;
	
	buf = _malloc(total_width + 3);
	if (!buf)
		goto oom;
	
	ptr1 = buf;
	list_for_each_entry(col, &c_list, entry) {
		n = strlen(col->column->name);
		if (col->width > n + 1) {
			ptr2 = ptr1;
			memset(ptr1, ' ', col->width/2 - n/2 + 1);
			ptr1 += col->width/2 - n/2 + 1;
			sprintf(ptr1, "%s", col->column->name);
			ptr1 = strchr(ptr1, 0);
			memset(ptr1, ' ', col->width + 2 - (ptr1 - ptr2));
			ptr1 += col->width + 2 - (ptr1 - ptr2);
			*ptr1 = '|';
			ptr1++;
		} else if (col->width > n) {
			sprintf(ptr1, " %s  |", col->column->name);
			ptr1 = strchr(ptr1, 0);
		} else {
			sprintf(ptr1, " %s |", col->column->name);
			ptr1 = strchr(ptr1, 0);
		}
	}

	strcpy(ptr1 - 1, "\r\n");
	cli_send(cli, buf);

	ptr1 = buf;
	list_for_each_entry(col, &c_list, entry) {
		memset(ptr1, '-', col->width + 2);
		ptr1 += col->width + 2;
		*ptr1 = '+';
		ptr1++;
	}

	strcpy(ptr1 - 1, "\r\n");
	cli_send(cli, buf);

	while (!list_empty(&r_list)) {
		row = list_entry(r_list.next, typeof(*row), entry);
		ptr1 = buf;
		list_for_each_entry(cell, &row->cell_list, entry) {
			ptr2 = ptr1;
			sprintf(ptr1, " %s ", cell->buf);
			ptr1 = strchr(ptr1, 0);
			n = ptr1 - ptr2;
			if (n - 2 < cell->col->width) {
				memset(ptr1, ' ', cell->col->width + 2 - (ptr1 - ptr2));
				ptr1 += cell->col->width + 2 - (ptr1 - ptr2);
			}
			*ptr1 = '|';
			ptr1++;
		}
		strcpy(ptr1 - 1, "\r\n");
		cli_send(cli, buf);
		list_del(&row->entry);
		free_row(row);
	}

	_free(buf);

out:
	while (!list_empty(&c_list)) {
		col = list_entry(c_list.next, typeof(*col), entry);
		list_del(&col->entry);
		_free(col);
	}

	if (re)
		pcre_free(re);

	return CLI_CMD_OK;

oom:
	if (buf)
		_free(buf);

	while (!list_empty(&t_list)) {
		row = list_entry(t_list.next, typeof(*row), entry);
		list_del(&row->entry);
		free_row(row);
	}
	cli_send(cli, "out of memory");
	goto out;
}

static void print_ifname(const struct ppp_t *ppp, char *buf)
{
	snprintf(buf, CELL_SIZE, "%s", ppp->ifname);
}

static void print_username(const struct ppp_t *ppp, char *buf)
{
	if (ppp->username)
		snprintf(buf, CELL_SIZE, "%s", ppp->username);
}

static void print_ip(const struct ppp_t *ppp, char *buf)
{
	char str[17];
	u_inet_ntoa(ppp->peer_ipaddr, str);
	sprintf(buf, "%s", str);
}

static void print_type(const struct ppp_t *ppp, char *buf)
{
	snprintf(buf, CELL_SIZE, "%s", ppp->ctrl->name);
}

static void print_state(const struct ppp_t *ppp, char *buf)
{
	char *state;
	switch (ppp->state) {
		case PPP_STATE_STARTING:
			state = "start";
			break;
		case PPP_STATE_ACTIVE:
			state = "active";
			break;
		case PPP_STATE_FINISHING:
			state = "finish";
			break;
		default:
			state = "unk";
	}
	sprintf(buf, "%s", state);
}

static void print_uptime(const struct ppp_t *ppp, char *buf)
{
	time_t uptime;
	int day,hour,min,sec;
	char time_str[14];

	if (ppp->stop_time)
		uptime = ppp->stop_time - ppp->start_time;
	else {
		time(&uptime);
		uptime -= ppp->start_time;
	}

	day = uptime/ (24*60*60); uptime %= (24*60*60);
	hour = uptime / (60*60); uptime %= (60*60);
	min = uptime / 60;
	sec = uptime % 60;
	if (day)
		snprintf(time_str, 13, "%i.%02i:%02i:%02i", day, hour, min, sec);
	else
		snprintf(time_str, 13, "%02i:%02i:%02i", hour, min, sec);

	sprintf(buf, "%s", time_str);
}

static void print_calling_sid(const struct ppp_t *ppp, char *buf)
{
	snprintf(buf, CELL_SIZE, "%s", ppp->ctrl->calling_station_id);
}

static void print_called_sid(const struct ppp_t *ppp, char *buf)
{
	snprintf(buf, CELL_SIZE, "%s", ppp->ctrl->called_station_id);
}

static void print_sid(const struct ppp_t *ppp, char *buf)
{
	snprintf(buf, CELL_SIZE, "%s", ppp->sessionid);
}

void __init init(void)
{
	cli_register_simple_cmd2(show_ses_exec, show_ses_help, 2, "show", "sessions");
	
	cli_show_ses_register("ifname", "interface name", print_ifname);
	cli_show_ses_register("username", "user name", print_username);
	cli_show_ses_register("ip", "IP address", print_ip);
	cli_show_ses_register("type", "VPN type", print_type);
	cli_show_ses_register("state", "state of session", print_state);
	cli_show_ses_register("uptime", "uptime", print_uptime);
	cli_show_ses_register("calling-sid", "calling station id", print_calling_sid);
	cli_show_ses_register("called-sid", "called station id", print_called_sid);
	cli_show_ses_register("sid", "session id", print_sid);
}

