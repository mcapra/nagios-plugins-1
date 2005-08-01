/*
 * $Id$
 *
 * A simple interface to executing programs from other programs, using an
 * optimized and safe popen()-like implementation. It is considered safe
 * in that no shell needs to be spawned and the environment passed to the
 * execve()'d program is essentially empty.
 *
 *
 * The code in this file is a derivative of popen.c which in turn was taken
 * from "Advanced Programming for the Unix Environment" by W. Richard Stevens.
 *
 * Care has been taken to make sure the functions are async-safe. The one
 * function which isn't is np_runcmd_init() which it doesn't make sense to
 * call twice anyway, so the api as a whole should be considered async-safe.
 *
 */

/** includes **/
#include "runcmd.h"
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif

/** macros **/
#ifndef WEXITSTATUS
# define WEXITSTATUS(stat_val) ((unsigned)(stat_val) >> 8)
#endif

#ifndef WIFEXITED
# define WIFEXITED(stat_val) (((stat_val) & 255) == 0)
#endif

/* 4.3BSD Reno <signal.h> doesn't define SIG_ERR */
#if defined(SIG_IGN) && !defined(SIG_ERR)
# define SIG_ERR ((Sigfunc *)-1)
#endif

/* This variable must be global, since there's no way the caller
 * can forcibly slay a dead or ungainly running program otherwise.
 * Multithreading apps and plugins can initialize it (via NP_RUNCMD_INIT)
 * in an async safe manner PRIOR to calling np_runcmd() for the first time.
 *
 * The check for initialized values is atomic and can
 * occur in any number of threads simultaneously. */
static pid_t *np_pids = NULL;

/* If OPEN_MAX isn't defined, we try the sysconf syscall first.
 * If that fails, we fall back to an educated guess which is accurate
 * on Linux and some other systems. There's no guarantee that our guess is
 * adequate and the program will die with SIGSEGV if it isn't and the
 * upper boundary is breached. */
#ifdef OPEN_MAX
# define maxfd OPEN_MAX
#else
# ifndef _SC_OPEN_MAX /* sysconf macro unavailable, so guess */
#  define maxfd 256
# else
static int maxfd = 0;
# endif /* _SC_OPEN_MAX */
#endif /* OPEN_MAX */


/** prototypes **/
static int np_runcmd_open(const char *, int *, int *)
	__attribute__((__nonnull__(1, 2, 3)));

static int np_fetch_output(int, output *, int)
	__attribute__((__nonnull__(2)));

static int np_runcmd_close(int);

/* imported from utils.h */
extern void die (int, const char *, ...)
	__attribute__((__noreturn__,__format__(__printf__, 2, 3)));


/* this function is NOT async-safe. It is exported so multithreaded
 * plugins (or other apps) can call it prior to running any commands
 * through this api and thus achieve async-safeness throughout the api */
void np_runcmd_init(void)
{
#if !defined(OPEN_MAX) && defined(_SC_OPEN_MAX)
	if(!maxfd) {
		if((maxfd = sysconf(_SC_OPEN_MAX)) < 0) {
			/* possibly log or emit a warning here, since there's no
			 * guarantee that our guess at maxfd will be adequate */
			maxfd = 256;
		}
	}
#endif

	if(!np_pids) np_pids = calloc(maxfd, sizeof(pid_t));
}


/* Start running a command */
static int
np_runcmd_open(const char *cmdstring, int *pfd, int *pfderr)
{
	char *env[2];
	char *cmd = NULL;
	char **argv = NULL;
	char *str;
	int argc;
	size_t cmdlen;
	pid_t pid;
#ifdef RLIMIT_CORE
	struct rlimit limit;
#endif

	int i = 0;

	if(!np_pids) NP_RUNCMD_INIT;

	env[0] = strdup("LC_ALL=C");
	env[1] = '\0';

	/* if no command was passed, return with no error */
	if (cmdstring == NULL)
		return -1;

	/* make copy of command string so strtok() doesn't silently modify it */
	/* (the calling program may want to access it later) */
	cmdlen = strlen(cmdstring);
	cmd = malloc(cmdlen + 1);
	if (cmd == NULL) return -1;
	memcpy(cmd, cmdstring, cmdlen);

	/* This is not a shell, so we don't handle "???" */
	if (strstr (cmdstring, "\"")) return -1;

	/* allow single quotes, but only if non-whitesapce doesn't occur on both sides */
	if (strstr (cmdstring, " ' ") || strstr (cmdstring, "'''"))
		return -1;

	/* each arg must be whitespace-separated, so args can be a maximum
	 * of (len / 2) + 1. We add 1 extra to the mix for NULL termination */
	argc = (cmdlen >> 1) + 2;
	argv = calloc(sizeof(char *), argc);

	if (argv == NULL) {
		printf (_("Could not malloc argv array in popen()\n"));
		return -1;
	}

	/* get command arguments (stupidly, but fairly quickly) */
	while (cmd) {
		str = cmd;
		str += strspn (str, " \t\r\n"); /* trim any leading whitespace */

		if (strstr (str, "'") == str) {	/* handle SIMPLE quoted strings */
			str++;
			if (!strstr (str, "'")) return -1;	/* balanced? */
			cmd = 1 + strstr (str, "'");
			str[strcspn (str, "'")] = 0;
		}
		else {
			if (strpbrk (str, " \t\r\n")) {
				cmd = 1 + strpbrk (str, " \t\r\n");
				str[strcspn (str, " \t\r\n")] = 0;
			}
			else {
				cmd = NULL;
			}
		}

		if (cmd && strlen (cmd) == strspn (cmd, " \t\r\n"))
			cmd = NULL;

		argv[i++] = str;
	}

	if (pipe(pfd) < 0 || pipe(pfderr) < 0 || (pid = fork()) < 0)
		return -1; /* errno set by the failing function */

	/* child runs exceve() and _exit. */
	if (pid == 0) {
#ifdef 	RLIMIT_CORE
		/* the program we execve shouldn't leave core files */
		getrlimit (RLIMIT_CORE, &limit);
		limit.rlim_cur = 0;
		setrlimit (RLIMIT_CORE, &limit);
#endif
		close (pfd[0]);
		if (pfd[1] != STDOUT_FILENO) {
			dup2 (pfd[1], STDOUT_FILENO);
			close (pfd[1]);
		}
		close (pfderr[0]);
		if (pfderr[1] != STDERR_FILENO) {
			dup2 (pfderr[1], STDERR_FILENO);
			close (pfderr[1]);
		}

		/* close all descriptors in np_pids[]
		 * This is executed in a separate address space (pure child),
		 * so we don't have to worry about async safety */
		for (i = 0; i < maxfd; i++)
			if(np_pids[i] > 0)
				close (i);

		execve (argv[0], argv, env);
		_exit (0);
	}

	/* parent picks up execution here */
	/* close childs descriptors in our address space */
	close(pfd[1]);
	close(pfderr[1]);

	/* tag our file's entry in the pid-list and return it */
	np_pids[pfd[0]] = pid;

	return pfd[0];
}


static int
np_runcmd_close(int fd)
{
	int status;
	pid_t pid;

	/* make sure this fd was opened by popen() */
	if(fd < 0 || fd > maxfd || !np_pids || (pid = np_pids[fd]) == 0)
		return -1;

	np_pids[fd] = 0;
	if (close (fd) == -1) return -1;

	/* EINTR is ok (sort of), everything else is bad */
	while (waitpid (pid, &status, 0) < 0)
		if (errno != EINTR) return -1;

	/* return child's termination status */
	return (WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}


void
popen_timeout_alarm_handler (int signo)
{
	size_t i;

	if (signo == SIGALRM)
		puts(_("CRITICAL - Plugin timed out while executing system call\n"));

	if(np_pids) for(i = 0; i < maxfd; i++) {
		if(np_pids[i] != 0) kill(np_pids[i], SIGKILL);
	}

	exit (STATE_CRITICAL);
}


static int
np_fetch_output(int fd, output *op, int flags)
{
	size_t len = 0, i = 0;
	size_t rsf = 6, ary_size = 0; /* rsf = right shift factor, dec'ed uncond once */
	char *buf = NULL;
	int ret;
	char tmpbuf[4096];

	op->buf = NULL;
	op->buflen = 0;
	while((ret = read(fd, tmpbuf, sizeof(tmpbuf))) > 0) {
		len = (size_t)ret;
		op->buf = realloc(op->buf, op->buflen + len + 1);
		memcpy(op->buf + op->buflen, tmpbuf, len);
		op->buflen += len;
		i++;
	}

	if(ret < 0) {
		printf("read() returned %d: %s\n", ret, strerror(errno));
		return ret;
	}

	if(!op->buf || !op->buflen) return 0;

	/* some plugins may want to keep output unbroken */
	if(flags & RUNCMD_NO_ARRAYS)
		return op->buflen;

	/* and some may want both (*sigh*) */
	if(flags & RUNCMD_NO_ASSOC) {
		buf = malloc(op->buflen);
		memcpy(buf, op->buf, op->buflen);
	}
	else buf = op->buf;

	op->line = NULL;
	op->lens = NULL;
	len = i = 0;
	while(i < op->buflen) {
		/* make sure we have enough memory */
		if(len >= ary_size) {
			ary_size = op->buflen >> --rsf;
			op->line = realloc(op->line, ary_size * sizeof(char *));
			op->lens = realloc(op->lens, ary_size * sizeof(size_t));
		}

		/* set the pointer to the string */
		op->line[len] = &buf[i];

		/* hop to next newline or end of buffer */
		while(buf[i] != '\n' && i < op->buflen) i++;
		buf[i] = '\0';

		/* calculate the string length using pointer difference */
		op->lens[len] = (size_t)&buf[i] - (size_t)op->line[len];
		
		len++;
		i++;
	}

	return len;
}


int
np_runcmd(const char *cmd, output *out, output *err, int flags)
{
	int fd, pfd_out[2], pfd_err[2];

	/* initialize the structs */
	if(out) memset(out, 0, sizeof(output));
	if(err) memset(err, 0, sizeof(output));

	if((fd = np_runcmd_open(cmd, pfd_out, pfd_err)) == -1)
		die (STATE_UNKNOWN, _("Could not open pipe: %s\n"), cmd);

	if(out) out->lines = np_fetch_output(pfd_out[0], out, flags);
	if(err) err->lines = np_fetch_output(pfd_err[0], err, flags);

	return np_runcmd_close(fd);
}