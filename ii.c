/*
 * (C)opyright MMV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * (C)opyright MMV-MMVII Nico Golde <nico at ngolde dot de>
 * See LICENSE file for license details.
 */
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef PIPE_BUF /* FreeBSD don't know PIPE_BUF */
#define PIPE_BUF 4096
#endif

enum { TOK_NICKSRV = 0, TOK_USER, TOK_CMD, TOK_CHAN, TOK_ARG, TOK_TEXT, TOK_LAST };

typedef struct Nick Nick;
struct Nick {
	char *name;
	Nick *next;
};

typedef struct Channel Channel;
struct Channel {
	int fd;
	char *name;
	Nick *nicks;
	Channel *next;
};

#define PING_TIMEOUT 300
#define SERVER_PORT 6667
#define SSL_SERVER_PORT 6697
#define WRITE(con, mes) (use_ssl ? sslwrite(mes, strlen(mes)) : write(con->irc, mes, strlen(mes)))
#define READ(fd, buf) (from_server && use_ssl ? SSL_read(irc->sslHandle, buf, sizeof(char)) : read(fd, buf, sizeof(char)))

typedef struct {
        int irc;
        SSL *sslHandle;
        SSL_CTX *sslContext;
} conn;
conn *irc;
static int use_ssl;
static time_t last_response;
static Channel *channels = NULL;
static char *host = "irc.freenode.net";
static char nick[32];			/* might change while running */
static char path[_POSIX_PATH_MAX];
static char message[PIPE_BUF]; /* message buf used for communication */

static void usage() {
	fprintf(stderr, "%s",
			"ii - irc it - " VERSION "\n"
			"(C)opyright MMV-MMVI Anselm R. Garbe\n"
			"(C)opyright MMV-MMVII Nico Golde\n"
			"usage: ii [-i <irc dir>] [-s <host>] [-p <port>] [-e <encryption>]\n"
			"          [-n <nick>] [-k <password>] [-f <fullname>]\n");
	exit(EXIT_SUCCESS);
}
static char *striplower(char *s) {
	char *p = NULL;
	for(p = s; p && *p; p++) {
		if(*p == '/') *p = '_';
		*p = tolower(*p);
	}
	return s;
}

/* creates directories top-down, if necessary */
static void create_dirtree(const char *dir) {
	char tmp[256];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp),"%s",dir);
	len = strlen(tmp);
	if(tmp[len - 1] == '/')
		tmp[len - 1] = 0;
	for(p = tmp + 1; *p; p++)
		if(*p == '/') {
			*p = 0;
			mkdir(tmp, S_IRWXU);
			*p = '/';
		}
	mkdir(tmp, S_IRWXU);
}

static int get_filepath(char *filepath, size_t len, char *channel, char *file) {
	if(channel) {
		if(!snprintf(filepath, len, "%s/%s", path, striplower(channel)))
			return 0;
		create_dirtree(filepath);
		return snprintf(filepath, len, "%s/%s/%s", path, striplower(channel), file);
	}
	return snprintf(filepath, len, "%s/%s", path, file);
}

static void create_filepath(char *filepath, size_t len, char *channel, char *suffix) {
	if(!get_filepath(filepath, len, channel, suffix)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
}

static int open_channel(char *name) {
	static char infile[256];
	create_filepath(infile, sizeof(infile), name, "in");
	if(access(infile, F_OK) == -1)
		mkfifo(infile, S_IRWXU);
	return open(infile, O_RDONLY | O_NONBLOCK, 0);
}

static void print_out(char *channel, char *buf); // needs to be declared
static void add_channel(char *name) {
	Channel *c;
	int fd;

	for(c = channels; c; c = c->next)
		if(!strcmp(name, c->name))
			return; /* already handled */

	fd = open_channel(name);
	if(fd == -1) {
		printf("ii: exiting, cannot create in channel: %s\n", name);
		exit(EXIT_FAILURE);
	}
	c = calloc(1, sizeof(Channel));
	if(!c) {
		perror("ii: cannot allocate memory");
		exit(EXIT_FAILURE);
	}
	if(!channels) channels = c;
	else {
		c->next = channels;
		channels = c;
	}
	c->nicks = NULL;
	c->fd = fd;
	c->name = strdup(name);

	if(name[0] && !((name[0]=='#')||(name[0]=='&')||(name[0]=='+')||(name[0]=='!'))) {
		char msg[128];
		snprintf(msg, sizeof(msg), "-!- %s has joined %s", nick, name);
		print_out(name, msg);
	}
}

static void rm_channel(Channel *c) {
	Channel *p;
	Nick *n, *nn;
	if(channels == c) channels = channels->next;
	else {
		for(p = channels; p && p->next != c; p = p->next);
		if(p->next == c)
			p->next = c->next;
	}
	for(n = c->nicks; n; n = nn) {
		nn = n->next;
		free(n->name);
		free(n);
	}
	free(c->name);
	free(c);
}

void sslwrite(char * text, size_t len) {
	SSL_write(irc->sslHandle, text, len);
}

static void login(char *key, char *fullname) {
	if(key) snprintf(message, PIPE_BUF,
				"PASS %s\r\nNICK %s\r\nUSER %s localhost %s :%s\r\n", key,
				nick, nick, host, fullname ? fullname : nick);
	else snprintf(message, PIPE_BUF, "NICK %s\r\nUSER %s localhost %s :%s\r\n",
				nick, nick, host, fullname ? fullname : nick);

	WRITE(irc, message);	/* login */
}

conn *tcpopen(unsigned short port) {
	int fd;
	conn *c;
	struct sockaddr_in sin;
	struct hostent *hp = gethostbyname(host);

	memset(&sin, 0, sizeof(struct sockaddr_in));
	if(!hp) {
		perror("ii: cannot retrieve host information");
		exit(EXIT_FAILURE);
	}
	sin.sin_family = AF_INET;
	memcpy(&sin.sin_addr, hp->h_addr, hp->h_length);
	sin.sin_port = htons(port);
	if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ii: cannot create socket");
		exit(EXIT_FAILURE);
	}
	if(connect(fd, (const struct sockaddr *) &sin, sizeof(sin)) < 0) {
		perror("ii: cannot connect to host");
		exit(EXIT_FAILURE);
	}
	c = malloc(sizeof(conn));
	c->irc = fd;
	if(use_ssl) {
		c->sslHandle = NULL;
		c->sslContext = NULL;
		SSL_load_error_strings();
		SSL_library_init();
		c->sslContext = SSL_CTX_new(SSLv23_client_method());
		if(c->sslContext == NULL)
			ERR_print_errors_fp(stderr);
		c->sslHandle = SSL_new(c->sslContext);
		if(!SSL_set_fd(c->sslHandle, c->irc)
				|| (SSL_connect(c->sslHandle) != 1))
			ERR_print_errors_fp(stderr);
	}
	return c;
}

static size_t tokenize(char **result, size_t reslen, char *str, char delim) {
	char *p = NULL, *n = NULL;
	size_t i;

	if(!str)
		return 0;
	for(n = str; *n == ' '; n++);
	p = n;
	for(i = 0; *n != 0;) {
		if(i == reslen)
			return 0;
		if(i > TOK_CHAN - TOK_CMD && strtol(result[0], NULL, 10) > 0) delim=':'; /* workaround non-RFC compliant messages */
		if(*n == delim) {
			*n = 0;
			result[i++] = p;
			p = ++n;
		} else
			n++;
	}
	if(i<reslen && p < n && strlen(p))
		result[i++] = p;
	return i;				/* number of tokens */
}

static void print_out(char *channel, char *buf) {
	static char outfile[256], server[256], buft[18];
	FILE *out = NULL;
	time_t t = time(0);

	if(channel) snprintf(server, sizeof(server), "-!- %s", channel);
	if(strstr(buf, server)) channel="";
	create_filepath(outfile, sizeof(outfile), channel, "out");
	if(channel && channel[0]) add_channel(channel);
	if(!(out = fopen(outfile, "a"))) return;

	strftime(buft, sizeof(buft), "%F %R", localtime(&t));
	fprintf(out, "%s %s\n", buft, buf);
	fclose(out);
}

static Channel *lookup_chan(const char *name) {
	Channel *c;
	for(c = channels; c; c = c->next)
		if(!strcmp(name, c->name))
			return c;
	return NULL;
}

static void write_names(const char *channel) {
	Channel *c;
	Nick *n;
	static char outfile[256];
  	FILE *out = NULL;

	if(!(c = lookup_chan(channel))) return;
	
	create_filepath(outfile, sizeof(outfile), (char *)channel, "names");

	if(!(out = fopen(outfile, "w"))) return;

	for(n = c->nicks; n; n = n->next) {
		fprintf(out, "%s\n", n->name);
	}
	
	fclose(out);  
}

static void add_name(const char *chan, const char *name) {
	Channel *c;
	Nick *n;
	if(!(c = lookup_chan(chan))) return;
	for(n = c->nicks; n; n = n->next)
		if(!strcmp(name, n->name)) return;
	if(!(n = malloc(sizeof(Nick)))) {
	       	perror("ii: cannot allocate memory");
		exit(EXIT_FAILURE);
	}
	n->name = strdup(name);
	n->next = c->nicks;
	c->nicks = n;
	write_names(chan);
}

static int rm_name(const char *chan, const char *name) {
	Channel *c;
	Nick *n, *pn = NULL;
	if(!(c = lookup_chan(chan))) return 0;
	for(n = c->nicks; n; pn = n, n = n->next) {
		if(!strcmp(name, n->name)) {
			if(pn) pn->next = n->next;
			else c->nicks = n->next;
			free(n->name);
			free(n);
			return 1;
		}
	}
	return 0;
	write_names(chan);
}

static void proc_names(const char *chan, char *names) {
	char *p;
	if(!(p = strtok(names," "))) return;
	do {
		if(*p == '@' || *p == '+')
			p++;
		add_name(chan,p);
	} while((p = strtok(NULL," ")));
}

static void quit_name(const char *name, const char *user, const char *text) {
	Channel *c;
	for(c = channels; c; c = c->next) {
		if(c->name && rm_name(c->name, name)) {
			snprintf(message, PIPE_BUF, "-!- %s(%s) has quit \"%s\"", name, user, text ? text : "");
			print_out(c->name, message);
		}
	}
}

static void nick_name(const char *old, const char *new) {
	Channel *c;
	for(c = channels; c; c = c->next) {
		if(c->name && rm_name(c->name, old)) {
			add_name(c->name, new);
			snprintf(message, PIPE_BUF, "-!- %s changed nick to \"%s\"", old, new);
			print_out(c->name, message);
		}
	}
}

static void proc_channels_privmsg(char *channel, char *buf) {
	snprintf(message, PIPE_BUF, "<%s> %s", nick, buf);
	print_out(channel, message);
	snprintf(message, PIPE_BUF, "PRIVMSG %s :%s\r\n", channel, buf);
	WRITE(irc, message);
}

static void proc_channels_input(Channel *c, char *buf) {
	static char infile[256];
	char *p = NULL;

	if(buf[0] != '/' && buf[0] != 0) {
		proc_channels_privmsg(c->name, buf);
		return;
	}
	message[0] = '\0';
	switch (buf[1]) {
		case 'j':
			p = strchr(&buf[3], ' ');
			if(p) *p = 0;
			add_channel(&buf[3]);
			if((buf[3]=='#')||(buf[3]=='&')||(buf[3]=='+')||(buf[3]=='!')){
				if(p) snprintf(message, PIPE_BUF, "JOIN %s %s\r\n", &buf[3], p + 1); /* password protected channel */
				else snprintf(message, PIPE_BUF, "JOIN %s\r\n", &buf[3]);
			}
			else if(p) {
				proc_channels_privmsg(&buf[3], p + 1);
				return;
			}
			break;
		case 't':
			if(strlen(buf)>=3) snprintf(message, PIPE_BUF, "TOPIC %s :%s\r\n", c->name, &buf[3]);
			break;
		case 'a':
			if(strlen(buf)>=3){
				snprintf(message, PIPE_BUF, "-!- %s is away \"%s\"", nick, &buf[3]);
				print_out(c->name, message);
			}
			if(buf[2] == 0 || strlen(buf)<3) /* or used to make else part safe */
				snprintf(message, PIPE_BUF, "AWAY\r\n");
			else
				snprintf(message, PIPE_BUF, "AWAY :%s\r\n", &buf[3]);
			break;
		case 'n':
			if(strlen(buf)>=3){
				snprintf(nick, sizeof(nick),"%s", &buf[3]);
				snprintf(message, PIPE_BUF, "NICK %s\r\n", &buf[3]);
			}
			break;
		case 'l':
			if(c->name[0] == 0)
				return;
			if(buf[2] == ' ' && strlen(buf)>=3)
				snprintf(message, PIPE_BUF, "PART %s :%s\r\n", c->name, &buf[3]);
			else
				snprintf(message, PIPE_BUF,
						"PART %s :ii - 500 SLOC are too much\r\n", c->name);
			WRITE(irc, message);
			close(c->fd);
			create_filepath(infile, sizeof(infile), c->name, "in");
			unlink(infile);
			rm_channel(c);
			return;
			break;
		default:
			snprintf(message, PIPE_BUF, "%s\r\n", &buf[1]);
			break;
	}
	if (message[0] != '\0')
		WRITE(irc, message);
}

static void proc_server_cmd(char *buf) {
	char *argv[TOK_LAST], *cmd = NULL, *p = NULL;
	int i;

	if(!buf || *buf=='\0')
		return;

	for(i = 0; i < TOK_LAST; i++)
		argv[i] = NULL;
	/* <message>  ::= [':' <prefix> <SPACE> ] <command> <params> <crlf>
	   <prefix>   ::= <servername> | <nick> [ '!' <user> ] [ '@' <host> ]
	   <command>  ::= <letter> { <letter> } | <number> <number> <number>
	   <SPACE>    ::= ' ' { ' ' }
	   <params>   ::= <SPACE> [ ':' <trailing> | <middle> <params> ]
	   <middle>   ::= <Any *non-empty* sequence of octets not including SPACE
	   or NUL or CR or LF, the first of which may not be ':'>
	   <trailing> ::= <Any, possibly *empty*, sequence of octets not including NUL or CR or LF>
	   <crlf>     ::= CR LF */

	if(buf[0] == ':') {		/* check prefix */
		if (!(p = strchr(buf, ' '))) return;
		*p = 0;
		for(++p; *p == ' '; p++);
		cmd = p;
		argv[TOK_NICKSRV] = &buf[1];
		if((p = strchr(buf, '!'))) {
			*p = 0;
			argv[TOK_USER] = ++p;
		}
	} else
		cmd = buf;

	/* remove CRLFs */
	for(p = cmd; p && *p != 0; p++)
		if(*p == '\r' || *p == '\n')
			*p = 0;

	if((p = strchr(cmd, ':'))) {
		*p = 0;
		argv[TOK_TEXT] = ++p;
	}

	tokenize(&argv[TOK_CMD], TOK_LAST - TOK_CMD, cmd, ' ');

	if(!argv[TOK_CMD] || !strncmp("PONG", argv[TOK_CMD], 5)) {
		return;
	} else if(!strncmp("PING", argv[TOK_CMD], 5)) {
		snprintf(message, PIPE_BUF, "PONG %s\r\n", argv[TOK_TEXT]);
		WRITE(irc, message);
		return;
	} else if(!strncmp("353", argv[TOK_CMD], 4)) {
		p = strtok(argv[TOK_ARG]," ");
		if(!(p = strtok(NULL," ")))
			return;
		snprintf(message, PIPE_BUF, "%s%s", argv[TOK_ARG] ? argv[TOK_ARG] : "", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
		print_out(0, message);
		proc_names(p, argv[TOK_TEXT]);
		return;
	} else if(!argv[TOK_NICKSRV] || !argv[TOK_USER]) {	/* server command */
		snprintf(message, PIPE_BUF, "%s%s", argv[TOK_ARG] ? argv[TOK_ARG] : "", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
		print_out(0, message);
		return;
	} else if(!strncmp("ERROR", argv[TOK_CMD], 6))
		snprintf(message, PIPE_BUF, "-!- error %s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "unknown");
	else if(!strncmp("JOIN", argv[TOK_CMD], 5)) {
		if(argv[TOK_TEXT] != NULL){
			p = strchr(argv[TOK_TEXT], ' ');
			if(p)
				*p = 0;
		}
		argv[TOK_CHAN] = argv[TOK_TEXT];
		snprintf(message, PIPE_BUF, "-!- %s(%s) has joined %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT]);
		add_name(argv[TOK_CHAN],argv[TOK_NICKSRV]);
	} else if(!strncmp("PART", argv[TOK_CMD], 5)) {
		if (!strcmp(nick, argv[TOK_NICKSRV]))
			return;
		snprintf(message, PIPE_BUF, "-!- %s(%s) has left %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN]);
		rm_name(argv[TOK_CHAN],argv[TOK_NICKSRV]);
	} else if(!strncmp("MODE", argv[TOK_CMD], 5))
		snprintf(message, PIPE_BUF, "-!- %s changed mode/%s -> %s %s", argv[TOK_NICKSRV], argv[TOK_CMD + 1] ? argv[TOK_CMD + 1] : "" , argv[TOK_CMD + 2]? argv[TOK_CMD + 2] : "", argv[TOK_CMD + 3] ? argv[TOK_CMD + 3] : "");
	else if(!strncmp("QUIT", argv[TOK_CMD], 5)) {
		quit_name(argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT]);
		return;
	} else if(!strncmp("NICK", argv[TOK_CMD], 5)) {
		nick_name(argv[TOK_NICKSRV], argv[TOK_TEXT]);
		return;
	} else if(!strncmp("TOPIC", argv[TOK_CMD], 6))
		snprintf(message, PIPE_BUF, "-!- %s changed topic to \"%s\"", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("KICK", argv[TOK_CMD], 5)) {
		snprintf(message, PIPE_BUF, "-!- %s kicked %s (\"%s\")", argv[TOK_NICKSRV], argv[TOK_ARG], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
		rm_name(argv[TOK_CHAN],argv[TOK_NICKSRV]);
	} else if(!strncmp("NOTICE", argv[TOK_CMD], 7))
		snprintf(message, PIPE_BUF, "-!- \"%s\")", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("PRIVMSG", argv[TOK_CMD], 8))
		snprintf(message, PIPE_BUF, "<%s> %s", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	if(!argv[TOK_CHAN] || !strncmp(argv[TOK_CHAN], nick, strlen(nick)))
		print_out(argv[TOK_NICKSRV], message);
	else
		print_out(argv[TOK_CHAN], message);
}

static int read_line(int fd, size_t res_len, char *buf, int from_server) {
	size_t i = 0;
	char c = 0;
	do {
		if(READ(fd, &c) != sizeof(char))
			return -1;
		buf[i++] = c;
	}
	while(c != '\n' && i < res_len);
	buf[i - 1] = 0;			/* eliminates '\n' */
	return 0;
}

static void handle_channels_input(Channel *c) {
	static char buf[PIPE_BUF];
	if(read_line(c->fd, PIPE_BUF, buf, 0) == -1) {
		close(c->fd);
		int fd = open_channel(c->name);
		if(fd != -1)
			c->fd = fd;
		else
			rm_channel(c);
		return;
	}
	proc_channels_input(c, buf);
}

static void handle_server_output() {
	static char buf[PIPE_BUF];
	if(read_line(irc->irc, PIPE_BUF, buf, 1) == -1) {
		perror("ii: remote host closed connection");
		exit(EXIT_FAILURE);
	}
	proc_server_cmd(buf);
}

static void run() {
	Channel *c;
	int r, maxfd;
	fd_set rd;
	struct timeval tv;
	char ping_msg[512];

	snprintf(ping_msg, sizeof(ping_msg), "PING %s\r\n", host);
	for(;;) {
		FD_ZERO(&rd);
		maxfd = irc->irc;
		FD_SET(irc->irc, &rd);
		for(c = channels; c; c = c->next) {
			if(maxfd < c->fd)
				maxfd = c->fd;
			FD_SET(c->fd, &rd);
		}

		tv.tv_sec = 120;
		tv.tv_usec = 0;
		r = select(maxfd + 1, &rd, 0, 0, &tv);
		if(r < 0) {
			if(errno == EINTR)
				continue;
			perror("ii: error on select()");
			exit(EXIT_FAILURE);
		} else if(r == 0) {
			if(time(NULL) - last_response >= PING_TIMEOUT) {
				print_out(NULL, "-!- ii shutting down: ping timeout");
				exit(EXIT_FAILURE);
			}
			WRITE(irc, ping_msg);
			continue;
		}
		if(FD_ISSET(irc->irc, &rd)) {
			handle_server_output();
			last_response = time(NULL);
		}
		for(c = channels; c; c = c->next)
			if(FD_ISSET(c->fd, &rd))
				handle_channels_input(c);
	}
}

int main(int argc, char *argv[]) {
	int i;
	unsigned short port = SERVER_PORT;
	struct passwd *spw = getpwuid(getuid());
	char *key = NULL, *fullname = NULL;
	char prefix[_POSIX_PATH_MAX];

	if(!spw) {
		fprintf(stderr,"ii: getpwuid() failed\n");
		exit(EXIT_FAILURE);
	}
	snprintf(nick, sizeof(nick), "%s", spw->pw_name);
	snprintf(prefix, sizeof(prefix),"%s/irc", spw->pw_dir);
	if (argc <= 1 || (argc == 2 && argv[1][0] == '-' && argv[1][1] == 'h')) usage();

	for(i = 1; (i + 1 < argc) && (argv[i][0] == '-'); i++) {
		switch (argv[i][1]) {
			case 'i': snprintf(prefix,sizeof(prefix),"%s", argv[++i]); break;
			case 's': host = argv[++i]; break;
			case 'p': port = strtol(argv[++i], NULL, 10); break;
			case 'n': snprintf(nick,sizeof(nick),"%s", argv[++i]); break;
			case 'k': key = argv[++i]; break;
			case 'f': fullname = argv[++i]; break;
			case 'e': use_ssl = 1; break;
			default: usage(); break;
		}
	}
	if(use_ssl)
		port = port == SERVER_PORT ? SSL_SERVER_PORT : SERVER_PORT;
	irc = tcpopen(port);
	if(!snprintf(path, sizeof(path), "%s/%s", prefix, host)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
	create_dirtree(path);

	add_channel(""); /* master channel */
	login(key, fullname);
	run();

	return 0;
}
