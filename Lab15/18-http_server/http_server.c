#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "http.h"

#define MAX_FLOW_NUM  (20000)

#define SNDBUF_SIZE (8*1024)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 128
#define FULLNAME_LIMIT 256

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

struct file_cache
{
	char name[NAME_LIMIT];
	char fullname[FULLNAME_LIMIT];
	uint64_t size;
	char *file;
};

struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[NAME_LIMIT];			// file name
	long int fsize;					// file size
};

struct server_vars *svars = NULL;

const int backlog = 4096;

const char *www_main = "./www/";
static struct file_cache fcache[MAX_FILES];
static int nfiles;

static char *status_code_to_string(int scode)
{
	switch (scode) {
		case 200:
			return "OK";
			break;

		case 404:
			return "Not Found";
			break;
	}

	return NULL;
}

void clean_server_variable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->keep_alive = 0;
}

void close_connection(int sockid, struct server_vars *sv)
{
	close(sockid);
	clean_server_variable(sv);
}

int send_until_available(int sockid, struct server_vars *sv)
{
	int sent = 0;
	int ret = 1;
	while (ret > 0) {
#define MIN(x,y) (x<y?x:y)
		int len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
		if (len <= 0) {
			break;
		}
		ret = send(sockid, fcache[sv->fidx].file + sv->total_sent, len, 0);
		if (ret < 0) {
			printf("Connection closed with client.\n");
			break;
		}
		sent += ret;
		sv->total_sent += ret;
	}

	if (sv->total_sent >= fcache[sv->fidx].size) {
		if (sv->keep_alive) {
			clean_server_variable(sv);
		} else {
			close_connection(sockid, sv);
		}
	}

	return sent;
}

int handle_connection(int sockid, struct server_vars *sv)
{
	/* HTTP request handling */
	char buf[HTTP_HEADER_LEN];
	int rd = recv(sockid, buf, HTTP_HEADER_LEN, 0);
	if (rd <= 0) {
		return -1;
	}
	memcpy(sv->request + sv->recv_len, (char *)buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
	sv->recv_len += rd;
	sv->request[sv->recv_len] = '\0';
	sv->request_len = find_http_header(sv->request, sv->recv_len);
	if (sv->request_len <= 0) {
		fprintf(stderr, "Socket %d: Failed to parse HTTP request header.\n"
				"read bytes: %d, recv_len: %d, "
				"request_len: %d, strlen: %ld, request: \n%s\n", 
				sockid, rd, sv->recv_len, 
				sv->request_len, strlen(sv->request), sv->request);
		return -1;
	}

	char url[URL_LEN];
	http_get_url(sv->request, sv->request_len, url, URL_LEN);
	sprintf(sv->fname, "%s%s", www_main, url);

	sv->keep_alive = FALSE;
	char keepalive_str[128];
	if (http_header_str_val(sv->request, "Connection: ", 
				strlen("Connection: "), keepalive_str, 128)) {	
		if (strstr(keepalive_str, "Keep-Alive")) {
			sv->keep_alive = TRUE;
		} else if (strstr(keepalive_str, "Close")) {
			sv->keep_alive = FALSE;
		}
	}

	/* Find file in cache */
	int scode = 404;
	for (int i = 0; i < nfiles; i++) {
		if (strcmp(sv->fname, fcache[i].fullname) == 0) {
			sv->fsize = fcache[i].size;
			sv->fidx = i;
			scode = 200;
			break;
		}
	}

	/* Response header handling */
	time_t t_now;
	char t_str[128];
	time(&t_now);
	strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
	if (sv->keep_alive)
		sprintf(keepalive_str, "Keep-Alive");
	else
		sprintf(keepalive_str, "Close");

	char response[HTTP_HEADER_LEN];
	sprintf(response, "HTTP/1.1 %d %s\r\n"
			"Date: %s\r\n"
			"Server: HTTP Server on TCP Stack\r\n"
			"Content-Length: %ld\r\n"
			"Connection: %s\r\n\r\n", 
			scode, status_code_to_string(scode), t_str, sv->fsize, keepalive_str);

	int len = strlen(response);
	int sent = 0;
	while (sent < len) {
		int ret = send(sockid, response+sent, len-sent, 0);
		if (ret < 0) {
			fprintf(stderr, "encounter error while sending.");
			return -1;
		}
		sent += ret;
	}

	send_until_available(sockid, sv);

	return 0;
}

int accept_connection(int listener)
{
	int c = accept(listener, NULL, NULL);
	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			fprintf(stderr, "Invalid socket id %d.\n", c);
			return -1;
		}

		struct server_vars *sv = &svars[c];
		clean_server_variable(sv);
	} else {
		fprintf(stderr, "accept() error.\n");
	}

	return c;
}

void init_server_vars()
{
	/* allocate memory for server variables */
	svars = (struct server_vars *)calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!svars) {
		fprintf(stderr, "Failed to create server_vars struct!\n");
		exit(-1);
	}
}

int create_listening_socket()
{
	/* create socket and set it as nonblocking */
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		fprintf(stderr, "Failed to create listening socket!\n");
		exit(-1);
	}

	struct sockaddr_in saddr;
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(80);
	int ret = bind(listener, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		fprintf(stderr, "Failed to bind to the listening socket!\n");
		exit(-1);
	}

	/* listen (backlog: can be configured) */
	ret = listen(listener, backlog);
	if (ret < 0) {
		fprintf(stderr, "listen() failed!\n");
		exit(-1);
	}

	return listener;
}

void *run_server_thread(void *arg)
{
	int fd = *(int *)arg;
	struct server_vars *sv = &svars[fd];

	while (TRUE) {
		handle_connection(fd, sv);
		if (sv->keep_alive)
			continue;
		else
			break;
	}

	close_connection(fd, sv);

	return NULL;
}

void init_server_cache()
{
	DIR *dir = opendir(www_main);
	if (dir == NULL) {
		fprintf(stdout, "dir '%s' does not exist!\n", www_main);
		exit(-1);
	}

	nfiles = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0)
			continue;
		else if (strcmp(ent->d_name, "..") == 0)
			continue;

		snprintf(fcache[nfiles].name, NAME_LIMIT, "%s", ent->d_name);
		snprintf(fcache[nfiles].fullname, FULLNAME_LIMIT, "%s/%s", www_main, ent->d_name);

		int fd = open(fcache[nfiles].fullname, O_RDONLY);
		if (fd < 0) {
			perror("open");
			continue;
		} else {
			fcache[nfiles].size = lseek64(fd, 0, SEEK_END);
			lseek64(fd, 0, SEEK_SET);
		}

		fcache[nfiles].file = (char *)malloc(fcache[nfiles].size);
		if (!fcache[nfiles].file) {
			fprintf(stdout, "Failed to allocate memory for file %s\n", fcache[nfiles].name);
			perror("malloc");
			continue;
		}

		fprintf(stdout, "Reading %s (%lu bytes)\n", fcache[nfiles].name, fcache[nfiles].size);
		uint64_t total_read = 0;
		while (1) {
			int ret = read(fd, fcache[nfiles].file + total_read, fcache[nfiles].size - total_read);
			if (ret < 0) {
				break;
			} else if (ret == 0) {
				break;
			}
			total_read += ret;
		}
		if (total_read < fcache[nfiles].size) {
			free(fcache[nfiles].file);
			continue;
		}
		close(fd);
		nfiles++;

		if (nfiles >= MAX_FILES)
			break;
	}
}

void run_server()
{
	int listener = create_listening_socket();

	while (TRUE) {
		int ret = accept_connection(listener);
		if (ret < 0)
			break;

		int cfd = ret;
		pthread_t pid;
		pthread_create(&pid, NULL, run_server_thread, (void *)&cfd);
	}
}

int main(int argc, char **argv)
{
	init_server_cache();

	init_server_vars();

	fprintf(stdout, "Application initialization finished.\n");

	run_server();
	
	return 0;
}
