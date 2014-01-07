/*
 * Simple UDP logger - A utility for receiving output from netconsole.
 *
 *    Written by Tetsuo Handa <penguin-kernel@I-love.SAKURA.ne.jp>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#define round_up(size) ((((size) + 4095u) / 4096u) * 4096u)

/* Structure for tracking partially received data. */
static struct client {
	struct sockaddr_in addr; /* Sender's IPv4 address and port. */
	char *buffer; /* Buffer for holding received data. */
	int avail; /* Valid bytes in @buffer . */
	char addr_str[24]; /* String representation of @addr . */
	time_t stamp; /* Timestamp of receiving the first byte in @buffer . */
} *clients = NULL;

/* Current clients. */
static int num_clients = 0;
/* Max clients. */
static int max_clients = 1024;
/* Max write buffer per a client. */
static int wbuf_size = 65536;
/* Max seconds to wait for new line. */
static int wait_timeout = 10;
/* Try to release unused memory? */
static _Bool try_drop_memory_usage = 0;
/* Handle for today's log file. */
static FILE *log_fp = NULL;
/* Previous time. */
static struct tm last_tm = { .tm_year = 70, .tm_mday = 1 };

/**
 * switch_logfile - Close yesterday's log file and open today's log file.
 *
 * @tm: Pointer to "struct tm" holding current time.
 *
 * Returns nothing.
 */
static void switch_logfile(struct tm *tm)
{
    /* Name of today's log file. */
    static char filename[16] = { };

	FILE *fp = log_fp;
	snprintf(filename, sizeof(filename) - 1, "%04u-%02u-%02u.log",
		 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
	log_fp = fopen(filename, "a");
	if (!fp)
		return;
	/* If open() failed, continue using old one. */
	if (log_fp)
		fclose(fp);
	else
		log_fp = fp;
	try_drop_memory_usage = 1;
}

/**
 * write_logfile - Write to today's log file.
 *
 * @ptr:    Pointer to "struct client".
 * @forced: True if the partial line should be written.
 *
 * Returns nothing.
 */
static void write_logfile(struct client *ptr, const _Bool forced)
{
	static time_t last_time = 0;
	static char stamp[24] = { };
	char *buffer = ptr->buffer;
	int avail = ptr->avail;
	const time_t now_time = ptr->stamp;
	if (last_time != now_time) {
		struct tm *tm = localtime(&now_time);
		if (!tm)
			tm = &last_tm;
		snprintf(stamp, sizeof(stamp) - 1, "%04u-%02u-%02u "
			 "%02u:%02u:%02u ", tm->tm_year + 1900, tm->tm_mon + 1,
			 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
		/*
		 * Switch log file if the day has changed. We can't use
		 * (last_time / 86400 != now_time / 86400) in order to allow
		 * switching at 00:00:00 of the local time.
		 */
		if (tm->tm_mday != last_tm.tm_mday ||
		    tm->tm_mon != last_tm.tm_mon ||
		    tm->tm_year != last_tm.tm_year) {
			last_tm = *tm;
			switch_logfile(tm);
		}
		last_time = now_time;
	}
	/* Write the completed lines. */
	while (1) {
		char *cp = memchr(buffer, '\n', avail);
		const int len = cp - buffer + 1;
		if (!cp)
			break;
		fprintf(log_fp, "%s%s", stamp, ptr->addr_str);
		fwrite(buffer, 1, len, log_fp);
		avail -= len;
		buffer += len;
	}
	/* Write the incomplete line if forced. */
	if (forced && avail) {
		fprintf(log_fp, "%s%s", stamp, ptr->addr_str);
		fwrite(buffer, 1, avail, log_fp);
		fprintf(log_fp, "\n");
		avail = 0;
	}
	/* Discard the written data. */
	if (ptr->buffer != buffer)
		memmove(ptr->buffer, buffer, avail);
	ptr->avail = avail;
}

/**
 * drop_memory_usage - Try to reduce memory usage.
 *
 * Returns nothing.
 */
static void drop_memory_usage(void)
{
	struct client *ptr;
	int i = 0;
	if (!try_drop_memory_usage)
		return;
	try_drop_memory_usage = 0;
	while (i < num_clients) {
		ptr = &clients[i];
		if (ptr->avail) {
			char *tmp = realloc(ptr->buffer, round_up(ptr->avail));
			if (tmp)
				ptr->buffer = tmp;
			i++;
			continue;
		}
		free(ptr->buffer);
		num_clients--;
		memmove(ptr, ptr + 1, (num_clients - i) * sizeof(*ptr));
	}
	if (num_clients) {
		ptr = realloc(clients, round_up(sizeof(*ptr) * num_clients));
		if (ptr)
			clients = ptr;
	} else {
		free(clients);
		clients = NULL;
	}
}

/**
 * flush_all_and_abort - Clean up upon out of memory.
 *
 * This function does not return.
 */
static void flush_all_and_abort(void)
{
	int i;
	for (i = 0; i < num_clients; i++)
		if (clients[i].avail) {
			write_logfile(&clients[i], 1);
			free(clients[i].buffer);
		}
	fprintf(log_fp, "[aborted due to memory allocation failure]\n");
	fflush(log_fp);
	exit(1);
}

/**
 * find_client - Find the structure for given address.
 *
 * @addr: Pointer to "struct sockaddr_in".
 *
 * Returns "struct client" for @addr on success, NULL otherwise.
 */
static struct client *find_client(struct sockaddr_in *addr)
{
	struct client *ptr;
	int i;
	for (i = 0; i < num_clients; i++)
		if (!memcmp(&clients[i].addr, addr, sizeof(*addr)))
			return &clients[i];
	if (i >= max_clients) {
		try_drop_memory_usage = 1;
		drop_memory_usage();
		if (i >= max_clients)
			return NULL;
	}
	ptr = realloc(clients, round_up(sizeof(*ptr) * (num_clients + 1)));
	if (!ptr)
		return NULL;
	clients = ptr;
	ptr = &clients[num_clients++];
	memset(ptr, 0, sizeof(*ptr));
	ptr->addr = *addr;
	snprintf(ptr->addr_str, sizeof(ptr->addr_str) - 1, "%s:%u ",
		 inet_ntoa(addr->sin_addr), htons(addr->sin_port));
	return ptr;
}

/**
 * do_main - The main loop.
 *
 * @fd: Receiver socket's file descriptor.
 *
 * Returns nothing.
 */
static void do_main(const int fd)
{
	static char buf[65536];
	struct sockaddr_in addr;
	while (1) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		socklen_t size = sizeof(addr);
		int i;
		time_t now;
		/* Don't wait forever if checking for timeout. */
		for (i = 0; i < num_clients; i++)
			if (clients[i].avail)
				break;
		/* Flush log file and wait for data. */
		fflush(log_fp);
		poll(&pfd, 1, i < num_clients ? 1000 : -1);
		now = time(NULL);
		/* Check for timeout. */
		for (i = 0; i < num_clients; i++)
			if (clients[i].avail &&
			    now - clients[i].stamp >= wait_timeout)
				write_logfile(&clients[i], 1);
		/* Don't receive forever in order to check for timeout. */
		while (now == time(NULL)) {
			struct client *ptr;
			char *tmp;
			int len = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
					   (struct sockaddr *) &addr, &size);
			if (len <= 0 || size != sizeof(addr))
				break;
			ptr = find_client(&addr);
			if (!ptr)
				continue;
			/* Save current time if receiving the first byte. */
			if (!ptr->avail)
				ptr->stamp = now;
			/* Append data to the line. */
			tmp = realloc(ptr->buffer, round_up(ptr->avail + len));
			if (!tmp)
				flush_all_and_abort();
			memmove(tmp + ptr->avail, buf, len);
			ptr->avail += len;
			ptr->buffer = tmp;
			/* Write if at least one line completed. */
			if (memchr(buf, '\n', len))
				write_logfile(ptr, 0);
			/* Write if the line is too long. */
			if (ptr->avail >= wbuf_size)
				write_logfile(ptr, 1);
		}
		drop_memory_usage();
	}
}

/**
 * usage - Print usage and exit.
 *
 * @name: Program's name.
 *
 * This function does not return.
 */
static void usage(const char *name)
{
	fprintf(stderr, "Simple UDP logger\n\n"
		"Usage:\n  %s [ip=$listen_ip] [port=$listen_port] "
		"[dir=$log_dir] [timeout=$seconds_waiting_for_newline] "
		"[clients=$max_clients] [wbuf=$write_buffer_size] "
		"[rbuf=$receive_buffer_size]\n\n"
		"The value of $seconds_waiting_for_newline should be between "
		"5 and 600.\nThe value of $max_clients should be between 10 "
		"and 65536.\nThe value of $write_buffer_size should be "
		"between 1024 and 1048576.\nThe value of $receive_buffer_size "
		"should be 65536 and 1073741824 (though actual size might be "
		"adjusted by the kernel).\n", name);
	exit (1);
}

/**
 * do_init - Initialization function.
 *
 * @argc: Number of arguments.
 * @argv: Arguments.
 *
 * Returns the listener socket's file descriptor.
 */
static int do_init(int argc, char *argv[])
{
	struct sockaddr_in addr = { };
	char pwd[4096];
	/* Max receive buffer size. */
	int rbuf_size = 8 * 1048576;
	socklen_t size;
	int fd;
	int i;
	/* Directory to save logs. */
	const char *log_dir = ".";
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(6666);
	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!strncmp(arg, "ip=", 3))
			addr.sin_addr.s_addr = inet_addr(arg + 3);
		else if (!strncmp(arg, "port=", 5))
			addr.sin_port = htons(atoi(arg + 5));
		else if (!strncmp(arg, "dir=", 4))
			log_dir = arg + 4;
		else if (!strncmp(arg, "timeout=", 8))
			wait_timeout = atoi(arg + 8);
		else if (!strncmp(arg, "clients=", 8))
			max_clients = atoi(arg + 8);
		else if (!strncmp(arg, "wbuf=", 5))
			wbuf_size = atoi(arg + 5);
		else if (!strncmp(arg, "rbuf=", 5))
			rbuf_size = atoi(arg + 5);
		else
			usage(argv[0]);
	}
	/* Sanity check. */
	if (max_clients < 10)
		max_clients = 10;
	if (max_clients > 65536)
		max_clients = 65536;
	if (wait_timeout < 5)
		wait_timeout = 5;
	if (wait_timeout > 600)
		wait_timeout = 600;
	if (wbuf_size < 1024)
		wbuf_size = 1024;
	if (wbuf_size > 1048576)
		wbuf_size = 1048576;
	if (rbuf_size < 65536)
		rbuf_size = 65536;
	if (rbuf_size > 1024 * 1048576)
		rbuf_size = 1024 * 1048576;
	/* Create the listener socket and configure it. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef SO_RCVBUFFORCE
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rbuf_size,
		       sizeof(rbuf_size))) {
#endif
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rbuf_size,
			       sizeof(rbuf_size))) {
			fprintf(stderr, "Can't set receive buffer size.\n");
			exit(1);
		}
#ifdef SO_RCVBUFFORCE
	}
#endif
	size = sizeof(rbuf_size);
	if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rbuf_size, &size)) {
		fprintf(stderr, "Can't get receive buffer size.\n");
		exit(1);
	}
	size = sizeof(addr);
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) ||
	    getsockname(fd, (struct sockaddr *) &addr, &size) ||
	    size != sizeof(addr)) {
		fprintf(stderr, "Can't bind to %s:%u .\n",
			inet_ntoa(addr.sin_addr), htons(addr.sin_port));
		exit(1);
	}
	/* Open the initial log file. */
	memset(pwd, 0, sizeof(pwd));
	if (chdir(log_dir) || !getcwd(pwd, sizeof(pwd) - 1)) {
		fprintf(stderr, "Can't change directory to %s .\n", log_dir);
		exit(1);
	} else {
		const time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		if (!tm)
			tm = &last_tm;
		switch_logfile(tm);
		if (!log_fp) {
			fprintf(stderr, "Can't create log file.\n");
			exit(1);
		}
		printf("Started at %04u-%02u-%02u %02u:%02u:%02u at %s\n",
		       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		       tm->tm_hour, tm->tm_min, tm->tm_sec, pwd);
	}
	/* Successfully initialized. */
	printf("Options: ip=%s port=%u dir=%s timeout=%u clients=%u wbuf=%u "
	       "rbuf=%u\n", inet_ntoa(addr.sin_addr), htons(addr.sin_port),
	       pwd, wait_timeout, max_clients, wbuf_size, rbuf_size);
	return fd;
}

int main(int argc, char *argv[])
{
	const int fd = do_init(argc, argv);
	do_main(fd);
	return 0;
}
