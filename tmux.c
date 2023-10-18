/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <getopt.h>
#include <langinfo.h>
#include <locale.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"
#include "tmate.h"

struct options	*global_options;	/* server options */
struct options	*global_s_options;	/* session options */
struct options	*global_w_options;	/* window options */
struct environ	*global_environ;
struct hooks	*global_hooks;

struct timeval	 start_time;
const char	*socket_path;

__dead void	 usage(void);
static char	*make_label(const char *);

#ifndef HAVE___PROGNAME
char      *__progname = (char *) "tmate";
#endif

#ifdef TMATE
int tmate_foreground;
#endif

__dead void
usage(void)
{
	fprintf(stderr,
	    "Usage: %s [options] [tmux-command [flags]]\n"
	    "\n"
	    "Basic options:\n"
	    " -n <name>    specify the session token instead of getting a random one\n"
	    " -r <name>    same, but for the read-only token\n"
	    " -k <key>     specify an api-key, necessary for using named sessions on tmate.io\n"
	    " -F           set the foreground mode, useful for setting remote access\n"
	    " -f <path>    set the config file path\n"
	    " -S <path>    set the socket path, useful to issue commands to a running tmate instance\n"
	    " -a <path>    limit access to ssh public keys listed in provided file\n"
	    " -v           set verbosity (can be repeated)\n"
	    " -V           print version\n"
	    ,__progname);
	exit(1);
}

const char *
getshell(void)
{
	struct passwd	*pw;
	const char	*shell;

	shell = getenv("SHELL");
	if (checkshell(shell))
		return (shell);

	pw = getpwuid(getuid());
	if (pw != NULL && checkshell(pw->pw_shell))
		return (pw->pw_shell);

	return (_PATH_BSHELL);
}

int
checkshell(const char *shell)
{
	if (shell == NULL || *shell == '\0' || *shell != '/')
		return (0);
	if (areshell(shell))
		return (0);
	if (access(shell, X_OK) != 0)
		return (0);
	return (1);
}

int
areshell(const char *shell)
{
	const char	*progname, *ptr;

	if ((ptr = strrchr(shell, '/')) != NULL)
		ptr++;
	else
		ptr = shell;
	progname = __progname;
	if (*progname == '-')
		progname++;
	if (strcmp(ptr, progname) == 0)
		return (1);
	return (0);
}

static char *
make_label(const char *label)
{
	char		*base, resolved[PATH_MAX], *path, *s;
	struct stat	 sb;
	u_int		 uid;
	int		 saved_errno;
#ifdef TMATE
	int do_random_label = label == NULL;
#endif

	if (label == NULL)
		label = "default";

	uid = getuid();

	if ((s = getenv("TMUX_TMPDIR")) != NULL && *s != '\0')
		xasprintf(&base, "%s/tmate-%u", s, uid);
	else
		xasprintf(&base, "%s/tmate-%u", _PATH_TMP, uid);

	if (mkdir(base, S_IRWXU) != 0 && errno != EEXIST)
		goto fail;

	if (lstat(base, &sb) != 0)
		goto fail;
	if (!S_ISDIR(sb.st_mode)) {
		errno = ENOTDIR;
		goto fail;
	}
	if (sb.st_uid != uid || (sb.st_mode & S_IRWXO) != 0) {
		errno = EACCES;
		goto fail;
	}

#ifdef TMATE
	if (do_random_label)
		label = "XXXXXX";
#endif

	if (realpath(base, resolved) == NULL)
		strlcpy(resolved, base, sizeof resolved);
	xasprintf(&path, "%s/%s", resolved, label);
#ifdef TMATE
	if (do_random_label)
		mktemp(path);
#endif
	return (path);

fail:
	saved_errno = errno;
	free(base);
	errno = saved_errno;
	return (NULL);
}

void
setblocking(int fd, int state)
{
	int mode;

	if ((mode = fcntl(fd, F_GETFL)) != -1) {
		if (!state)
			mode |= O_NONBLOCK;
		else
			mode &= ~O_NONBLOCK;
		fcntl(fd, F_SETFL, mode);
	}
}

const char *
find_home(void)
{
	struct passwd		*pw;
	static const char	*home;

	if (home != NULL)
		return (home);

	home = getenv("HOME");
	if (home == NULL || *home == '\0') {
		pw = getpwuid(getuid());
		if (pw != NULL)
			home = pw->pw_dir;
		else
			home = NULL;
	}

	return (home);
}

#ifdef TMATE
static char *api_key;
static char *session_name;
static char *session_name_ro;
static char *authorized_keys;

void tmate_load_cli_options(void)
{
#define SET_OPT(name, val) ({\
	if (val) { \
		run_headless_command(3, (const char *[]){"set-option", name, val}, DEFER_ERRORS_CFG, NULL); \
		free(val); \
		val = NULL; \
	} \
})
	SET_OPT("tmate-api-key",         api_key);
	SET_OPT("tmate-session-name",    session_name);
	SET_OPT("tmate-session-name-ro", session_name_ro);
	SET_OPT("tmate-authorized-keys", authorized_keys);
#undef SET_OPT
}
#endif

int
main(int argc, char **argv)
{
	char		*path, *label, **var, tmp[PATH_MAX], *shellcmd = NULL;
	const char	*s;
	int		 opt, flags, keys;

	if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL &&
	    setlocale(LC_CTYPE, "C.UTF-8") == NULL) {
		if (setlocale(LC_CTYPE, "") == NULL)
			errx(1, "invalid LC_ALL, LC_CTYPE or LANG");
		s = nl_langinfo(CODESET);
		if (strcasecmp(s, "UTF-8") != 0 && strcasecmp(s, "UTF8") != 0)
			errx(1, "need UTF-8 locale (LC_CTYPE) but have %s", s);
	}

	setlocale(LC_TIME, "");
	tzset();

	if (**argv == '-')
		flags = CLIENT_LOGIN;
	else
		flags = 0;

#ifdef TMATE
	tmate_catch_sigsegv();
	flags |= CLIENT_256COLOURS | CLIENT_UTF8;
#endif

	label = path = NULL;
	while ((opt = getopt(argc, argv, "h2c:CdFf:lL:qS:uUVvk:n:r:a:")) != -1) {
		switch (opt) {
		case '2':
			flags |= CLIENT_256COLOURS;
			break;
		case 'c':
			free(shellcmd);
			shellcmd = xstrdup(optarg);
			break;
		case 'C':
			if (flags & CLIENT_CONTROL)
				flags |= CLIENT_CONTROLCONTROL;
			else
				flags |= CLIENT_CONTROL;
			break;
		case 'V':
			printf("%s %s\n", __progname, VERSION);
			printf("libssh %s\n", ssh_version(0));
			
			exit(0);
		case 'f':
			set_cfg_file(optarg);
			break;
		case 'l':
			flags |= CLIENT_LOGIN;
			break;
		case 'L':
			free(label);
			label = xstrdup(optarg);
			break;
		case 'q':
			break;
		case 'S':
			free(path);
			path = xstrdup(optarg);
			break;
		case 'u':
			flags |= CLIENT_UTF8;
			break;
		case 'v':
			log_add_level();
			break;
		case 'F':
			tmate_foreground = 1;
			log_add_level();
			unsetenv("TMUX");
			break;
		case 'k':
			api_key = xstrdup(optarg);
			break;
		case 'n':
			session_name = xstrdup(optarg);
			break;
		case 'r':
			session_name_ro = xstrdup(optarg);
			break;
		case 'a':
			authorized_keys = xstrdup(optarg);
			break;
		case 'h':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (shellcmd != NULL && argc != 0)
		usage();

#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath flock fattr unix getpw sendfd "
	    "recvfd proc exec tty ps", NULL) != 0)
		err(1, "pledge");
#endif

	/*
	 * tmux is a UTF-8 terminal, so if TMUX is set, assume UTF-8.
	 * Otherwise, if the user has set LC_ALL, LC_CTYPE or LANG to contain
	 * UTF-8, it is a safe assumption that either they are using a UTF-8
	 * terminal, or if not they know that output from UTF-8-capable
	 * programs may be wrong.
	 */
	if (getenv("TMUX") != NULL)
		flags |= CLIENT_UTF8;
	else {
		s = getenv("LC_ALL");
		if (s == NULL || *s == '\0')
			s = getenv("LC_CTYPE");
		if (s == NULL || *s == '\0')
			s = getenv("LANG");
		if (s == NULL || *s == '\0')
			s = "";
		if (strcasestr(s, "UTF-8") != NULL ||
		    strcasestr(s, "UTF8") != NULL)
			flags |= CLIENT_UTF8;
	}

	global_hooks = hooks_create(NULL);

	global_environ = environ_create();
	for (var = environ; *var != NULL; var++)
		environ_put(global_environ, *var);
	if (getcwd(tmp, sizeof tmp) != NULL)
		environ_set(global_environ, "PWD", "%s", tmp);

	global_options = options_create(NULL);
	options_table_populate_tree(OPTIONS_TABLE_SERVER, global_options);

	global_s_options = options_create(NULL);
	options_table_populate_tree(OPTIONS_TABLE_SESSION, global_s_options);
	options_set_string(global_s_options, "default-shell", "%s", getshell());

	global_w_options = options_create(NULL);
	options_table_populate_tree(OPTIONS_TABLE_WINDOW, global_w_options);

	/* Override keys to vi if VISUAL or EDITOR are set. */
	if ((s = getenv("VISUAL")) != NULL || (s = getenv("EDITOR")) != NULL) {
		if (strrchr(s, '/') != NULL)
			s = strrchr(s, '/') + 1;
		if (strstr(s, "vi") != NULL)
			keys = MODEKEY_VI;
		else
			keys = MODEKEY_EMACS;
		options_set_number(global_s_options, "status-keys", keys);
		options_set_number(global_w_options, "mode-keys", keys);
	}

	/*
	 * If socket is specified on the command-line with -S or -L, it is
	 * used. Otherwise, $TMUX is checked and if that fails "default" is
	 * used.
	 */
	if (path == NULL && label == NULL) {
		s = getenv("TMUX");
		if (s != NULL && *s != '\0' && *s != ',') {
			path = xstrdup(s);
			path[strcspn (path, ",")] = '\0';
		}
	}
	if (path == NULL && (path = make_label(label)) == NULL) {
		fprintf(stderr, "can't create socket: %s\n", strerror(errno));
		exit(1);
	}
	socket_path = path;
	free(label);

	/* Pass control to the client. */
	exit(client_main(event_init(), argc, argv, flags, shellcmd));
}
