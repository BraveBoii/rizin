// SPDX-FileCopyrightText: 2014-2020 pancake <pancake@nopcode.org>
// SPDX-License-Identifier: LGPL-3.0-only

/* this helper api is here because it depends on rz_util and rz_socket */
/* we should find a better place for it. rz_io? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <rz_socket.h>
#include <rz_util.h>
#include <rz_lib.h>
#include <rz_cons.h>
#include <sys/stat.h>
#include <sys/types.h>

#if __APPLE__ && HAVE_FORK
#include <spawn.h>
#include <sys/wait.h>
#include <mach/exception_types.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/vm_map.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
#if defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <pty.h>
#include <utmp.h>
#endif
#endif

#if __UNIX__
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <grp.h>
#include <errno.h>
#if defined(__sun)
#include <sys/filio.h>
#endif
#if __linux__ && !__ANDROID__
#include <sys/personality.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#if HAVE_DECL_PROCCTL_ASLR_CTL
#include <sys/procctl.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#ifdef _MSC_VER
#include <direct.h> // to compile chdir in msvc windows
#include <process.h> // to compile execv in msvc windows
#define pid_t int
#endif

#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
static int (*dyn_openpty)(int *amaster, int *aslave, char *name, struct termios *termp, struct winsize *winp) = NULL;
static int (*dyn_login_tty)(int fd) = NULL;
static id_t (*dyn_forkpty)(int *amaster, char *name, struct termios *termp, struct winsize *winp) = NULL;
static void dyn_init(void) {
	if (!dyn_openpty) {
		dyn_openpty = rz_lib_dl_sym(NULL, "openpty");
	}
	if (!dyn_login_tty) {
		dyn_openpty = rz_lib_dl_sym(NULL, "login_tty");
	}
	if (!dyn_forkpty) {
		dyn_openpty = rz_lib_dl_sym(NULL, "forkpty");
	}
}

#endif

RZ_API RzRunProfile *rz_run_new(const char *str) {
	RzRunProfile *p = RZ_NEW0(RzRunProfile);
	if (p) {
		rz_run_reset(p);
		if (str) {
			rz_run_parsefile(p, str);
		}
	}
	return p;
}

RZ_API void rz_run_reset(RzRunProfile *p) {
	rz_return_if_fail(p);
	memset(p, 0, sizeof(RzRunProfile));
	p->_aslr = -1;
}

RZ_API bool rz_run_parse(RzRunProfile *pf, const char *profile) {
	rz_return_val_if_fail(pf && profile, false);
	char *p, *o, *str = strdup(profile);
	if (!str) {
		return false;
	}
	rz_str_replace_char(str, '\r', 0);
	p = str;
	while (p) {
		if ((o = strchr(p, '\n'))) {
			*o++ = 0;
		}
		rz_run_parseline(pf, p);
		p = o;
	}
	free(str);
	return true;
}

RZ_API void rz_run_free(RzRunProfile *r) {
	if (r) {
		free(r->_system);
		free(r->_program);
		free(r->_runlib);
		free(r->_runlib_fcn);
		free(r->_stdio);
		free(r->_stdin);
		free(r->_stdout);
		free(r->_stderr);
		free(r->_chgdir);
		free(r->_chroot);
		free(r->_libpath);
		free(r->_preload);
		free(r);
	}
}

#if __UNIX__
static void set_limit(int n, int a, ut64 b) {
	if (n) {
		struct rlimit cl = { b, b };
		setrlimit(RLIMIT_CORE, &cl);
	} else {
		struct rlimit cl = { 0, 0 };
		setrlimit(a, &cl);
	}
}
#endif

static char *getstr(const char *src) {
	int len;
	char *ret = NULL;

	switch (*src) {
	case '\'':
		ret = strdup(src + 1);
		if (ret) {
			len = strlen(ret);
			if (len > 0) {
				len--;
				if (ret[len] == '\'') {
					ret[len] = 0;
					return ret;
				}
				eprintf("Missing \"\n");
			}
			free(ret);
		}
		return NULL;
	case '"':
		ret = strdup(src + 1);
		if (ret) {
			len = strlen(ret);
			if (len > 0) {
				len--;
				if (ret[len] == '"') {
					ret[len] = 0;
					rz_str_unescape(ret);
					return ret;
				}
				eprintf("Missing \"\n");
			}
			free(ret);
		}
		return NULL;
	case '@': {
		char *pat = strchr(src + 1, '@');
		if (pat) {
			size_t len;
			long i, rep;
			*pat++ = 0;
			rep = strtol(src + 1, NULL, 10);
			len = strlen(pat);
			if (rep > 0) {
				char *buf = malloc(rep);
				if (buf) {
					for (i = 0; i < rep; i++) {
						buf[i] = pat[i % len];
					}
				}
				return buf;
			}
		}
		// slurp file
		return rz_file_slurp(src + 1, NULL);
	}
	case '`': {
		char *msg = strdup(src + 1);
		int msg_len = strlen(msg);
		if (msg_len > 0) {
			msg[msg_len - 1] = 0;
			char *ret = rz_sys_cmd_str(msg, NULL, NULL);
			rz_str_trim_tail(ret);
			free(msg);
			return ret;
		}
		free(msg);
		return strdup("");
	}
	case '!': {
		char *a = rz_sys_cmd_str(src + 1, NULL, NULL);
		rz_str_trim_tail(a);
		return a;
	}
	case ':':
		if (src[1] == '!') {
			ret = rz_sys_cmd_str(src + 1, NULL, NULL);
			rz_str_trim_tail(ret); // why no head :?
		} else {
			ret = strdup(src);
		}
		len = rz_hex_str2bin(src + 1, (ut8 *)ret);
		if (len > 0) {
			ret[len] = 0;
			return ret;
		}
		eprintf("Invalid hexpair string\n");
		free(ret);
		return NULL;
	}
	rz_str_unescape((ret = strdup(src)));
	return ret;
}

static int parseBool(const char *e) {
	return (strcmp(e, "yes") ? (strcmp(e, "on") ? (strcmp(e, "true") ? (strcmp(e, "1") ? 0 : 1) : 1) : 1) : 1);
}

// TODO: move into rz_util? rz_run_... ? with the rest of funcs?
static void setASLR(RzRunProfile *r, int enabled) {
#if __linux__
	rz_sys_aslr(enabled);
#if HAVE_DECL_ADDR_NO_RANDOMIZE && !__ANDROID__
	if (personality(ADDR_NO_RANDOMIZE) == -1) {
#endif
		rz_sys_aslr(0);
#if HAVE_DECL_ADDR_NO_RANDOMIZE && !__ANDROID__
	}
#endif
#elif __APPLE__
	// TOO OLD setenv ("DYLD_NO_PIE", "1", 1);
	// disable this because its
	const char *argv0 = r->_system ? r->_system
		: r->_program          ? r->_program
		: r->_args[0]          ? r->_args[0]
				       : "/path/to/exec";
	eprintf("To disable aslr patch mach0.hdr.flags with:\n"
		"rizin -qwnc 'wx 000000 @ 0x18' %s\n",
		argv0);
	// f MH_PIE=0x00200000; wB-MH_PIE @ 24\n");
	// for osxver>=10.7
	// "unset the MH_PIE bit in an already linked executable" with --no-pie flag of the script
	// the right way is to disable the aslr bit in the spawn call
#elif __FreeBSD__ || __NetBSD__ || __DragonFly__
	rz_sys_aslr(enabled);
#if HAVE_DECL_PROCCTL_ASLR_CTL
	int disabled = PROC_ASLR_FORCE_DISABLE;
	if (procctl(P_PID, getpid(), PROC_ASLR_CTL, &disabled) == -1) {
		rz_sys_aslr(0);
	}
#endif
#else
	// not supported for this platform
#endif
}

#if __APPLE__
#else
#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
static void restore_saved_fd(int saved, bool restore, int fd) {
	if (saved == -1) {
		return;
	}
	if (restore) {
		dup2(saved, fd);
	}
	close(saved);
}
#endif

static int handle_redirection_proc(const char *cmd, bool in, bool out, bool err) {
#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
	if (!dyn_forkpty) {
		// No forkpty api found, maybe we should fallback to just fork without any pty allocated
		return -1;
	}
	// use PTY to redirect I/O because pipes can be problematic in
	// case of interactive programs.
	int saved_stdin = dup(STDIN_FILENO);
	if (saved_stdin == -1) {
		return -1;
	}
	int saved_stdout = dup(STDOUT_FILENO);
	if (saved_stdout == -1) {
		close(saved_stdin);
		return -1;
	}

	int fdm, pid = dyn_forkpty(&fdm, NULL, NULL, NULL);
	if (pid == -1) {
		close(saved_stdin);
		close(saved_stdout);
		return -1;
	}
	const char *tn = ttyname(fdm);
	if (!tn) {
		close(saved_stdin);
		close(saved_stdout);
		return -1;
	}
	int fds = open(tn, O_RDWR);
	if (fds == -1) {
		close(saved_stdin);
		close(saved_stdout);
		return -1;
	}
	if (pid == 0) {
		close(fdm);
		// child process
		if (in) {
			dup2(fds, STDIN_FILENO);
		}
		if (out) {
			dup2(fds, STDOUT_FILENO);
		}
		// child - program to run

		// necessary because otherwise you can read the same thing you
		// wrote on fdm.
		struct termios t;
		tcgetattr(fds, &t);
		cfmakeraw(&t);
		tcsetattr(fds, TCSANOW, &t);

		int code = rz_sys_system(cmd);
		restore_saved_fd(saved_stdin, in, STDIN_FILENO);
		restore_saved_fd(saved_stdout, out, STDOUT_FILENO);
		exit(code);
	} else {
		close(fds);
		if (in) {
			dup2(fdm, STDIN_FILENO);
		}
		if (out) {
			dup2(fdm, STDOUT_FILENO);
		}
		// parent process
		int status;
		waitpid(pid, &status, 0);
	}

	// parent
	close(saved_stdin);
	close(saved_stdout);
	return 0;
#else
#ifdef _MSC_VER
#pragma message("TODO: handle_redirection_proc: Not implemented for this platform")
#else
#warning handle_redirection_proc : unimplemented for this platform
#endif
	return -1;
#endif
}
#endif

static int handle_redirection(const char *cmd, bool in, bool out, bool err) {
#if __APPLE__
	// XXX handle this in other layer since things changes a little bit
	// this seems like a really good place to refactor stuff
	return 0;
#else
	if (!cmd || !*cmd) {
		return 0;
	}
	if (cmd[0] == '"') {
#if __UNIX__
		if (in) {
			int pipes[2];
			if (rz_sys_pipe(pipes, true) != -1) {
				size_t cmdl = strlen(cmd) - 2;
				if (write(pipes[1], cmd + 1, cmdl) != cmdl) {
					eprintf("[ERROR] rz-run: Cannot write to the pipe\n");
					close(0);
					return 1;
				}
				if (write(pipes[1], "\n", 1) != 1) {
					eprintf("[ERROR] rz-run: Cannot write to the pipe\n");
					close(0);
					return 1;
				}
				while ((dup2(pipes[0], STDIN_FILENO) == -1) && (errno == EINTR)) {
				}
				rz_sys_pipe_close(pipes[0]);
				rz_sys_pipe_close(pipes[1]);
			} else {
				eprintf("[ERROR] rz-run: Cannot create pipe\n");
			}
		}
#else
#ifdef _MSC_VER
#pragma message("string redirection handle not yet done")
#else
#warning quoted string redirection handle not yet done
#endif
#endif
	} else if (cmd[0] == '!') {
		// redirection to a process
		return handle_redirection_proc(cmd + 1, in, out, err);
	} else {
		// redirection to a file
		int f, flag = 0, mode = 0;
		flag |= in ? O_RDONLY : 0;
		flag |= out ? O_WRONLY | O_CREAT : 0;
		flag |= err ? O_WRONLY | O_CREAT : 0;
#ifdef __WINDOWS__
		mode = _S_IREAD | _S_IWRITE;
#else
		mode = S_IRUSR | S_IWUSR;
#endif
		f = open(cmd, flag, mode);
		if (f < 0) {
			eprintf("[ERROR] rz-run: Cannot open: %s\n", cmd);
			return 1;
		}
#define DUP(x) \
	{ \
		close(x); \
		dup2(f, x); \
	}
		if (in) {
			DUP(0);
		}
		if (out) {
			DUP(1);
		}
		if (err) {
			DUP(2);
		}
		close(f);
	}
	return 0;
#endif
}

RZ_API bool rz_run_parsefile(RzRunProfile *p, const char *b) {
	rz_return_val_if_fail(p && b, false);
	char *s = rz_file_slurp(b, NULL);
	if (s) {
		bool ret = rz_run_parse(p, s);
		free(s);
		return ret;
	}
	return 0;
}

RZ_API bool rz_run_parseline(RzRunProfile *p, const char *b) {
	int must_free = false;
	char *e = strchr(b, '=');
	if (!e || *b == '#') {
		return 0;
	}
	*e++ = 0;
	if (*e == '$') {
		must_free = true;
		e = rz_sys_getenv(e);
	}
	if (!e) {
		return 0;
	}
	if (!strcmp(b, "program")) {
		p->_args[0] = p->_program = strdup(e);
	} else if (!strcmp(b, "daemon")) {
		p->_daemon = true;
	} else if (!strcmp(b, "system")) {
		p->_system = strdup(e);
	} else if (!strcmp(b, "runlib")) {
		p->_runlib = strdup(e);
	} else if (!strcmp(b, "runlib.fcn")) {
		p->_runlib_fcn = strdup(e);
	} else if (!strcmp(b, "aslr")) {
		p->_aslr = parseBool(e);
	} else if (!strcmp(b, "pid")) {
		p->_pid = parseBool(e);
	} else if (!strcmp(b, "pidfile")) {
		p->_pidfile = strdup(e);
	} else if (!strcmp(b, "connect")) {
		p->_connect = strdup(e);
	} else if (!strcmp(b, "listen")) {
		p->_listen = strdup(e);
	} else if (!strcmp(b, "pty")) {
		p->_pty = parseBool(e);
	} else if (!strcmp(b, "stdio")) {
		if (e[0] == '!') {
			p->_stdio = strdup(e);
		} else {
			p->_stdout = strdup(e);
			p->_stderr = strdup(e);
			p->_stdin = strdup(e);
		}
	} else if (!strcmp(b, "stdout")) {
		p->_stdout = strdup(e);
	} else if (!strcmp(b, "stdin")) {
		p->_stdin = strdup(e);
	} else if (!strcmp(b, "stderr")) {
		p->_stderr = strdup(e);
	} else if (!strcmp(b, "input")) {
		p->_input = strdup(e);
	} else if (!strcmp(b, "chdir")) {
		p->_chgdir = strdup(e);
	} else if (!strcmp(b, "core")) {
		p->_docore = parseBool(e);
	} else if (!strcmp(b, "fork")) {
		p->_dofork = parseBool(e);
	} else if (!strcmp(b, "sleep")) {
		p->_rzsleep = atoi(e);
	} else if (!strcmp(b, "maxstack")) {
		p->_maxstack = atoi(e);
	} else if (!strcmp(b, "maxproc")) {
		p->_maxproc = atoi(e);
	} else if (!strcmp(b, "maxfd")) {
		p->_maxfd = atoi(e);
	} else if (!strcmp(b, "bits")) {
		p->_bits = atoi(e);
	} else if (!strcmp(b, "chroot")) {
		p->_chroot = strdup(e);
	} else if (!strcmp(b, "libpath")) {
		p->_libpath = strdup(e);
	} else if (!strcmp(b, "preload")) {
		p->_preload = strdup(e);
	} else if (!strcmp(b, "rzpreload")) {
		p->_rzpreload = parseBool(e);
	} else if (!strcmp(b, "rzpreweb")) {
		rz_sys_setenv("RZ_RUN_WEB", "yes");
	} else if (!strcmp(b, "setuid")) {
		p->_setuid = strdup(e);
	} else if (!strcmp(b, "seteuid")) {
		p->_seteuid = strdup(e);
	} else if (!strcmp(b, "setgid")) {
		p->_setgid = strdup(e);
	} else if (!strcmp(b, "setegid")) {
		p->_setegid = strdup(e);
	} else if (!strcmp(b, "nice")) {
		p->_nice = atoi(e);
	} else if (!strcmp(b, "timeout")) {
		p->_timeout = atoi(e);
	} else if (!strcmp(b, "timeoutsig")) {
		p->_timeout_sig = rz_signal_from_string(e);
	} else if (!memcmp(b, "arg", 3)) {
		int n = atoi(b + 3);
		if (n >= 0 && n < RZ_RUN_PROFILE_NARGS) {
			p->_args[n] = getstr(e);
			p->_argc++;
		} else {
			eprintf("Out of bounds args index: %d\n", n);
		}
	} else if (!strcmp(b, "envfile")) {
		char *p, buf[1024];
		size_t len;
		FILE *fd = rz_sys_fopen(e, "r");
		if (!fd) {
			eprintf("Cannot open '%s'\n", e);
			if (must_free == true) {
				free(e);
			}
			return false;
		}
		for (;;) {
			if (!fgets(buf, sizeof(buf), fd)) {
				break;
			}
			if (feof(fd)) {
				break;
			}
			p = strchr(buf, '=');
			if (p) {
				*p++ = 0;
				len = strlen(p);
				if (len > 0 && p[len - 1] == '\n') {
					p[len - 1] = 0;
				}
				if (len > 1 && p[len - 2] == '\r') {
					p[len - 2] = 0;
				}
				rz_sys_setenv(buf, p);
			}
		}
		fclose(fd);
	} else if (!strcmp(b, "unsetenv")) {
		rz_sys_setenv(e, NULL);
	} else if (!strcmp(b, "setenv")) {
		char *V, *v = strchr(e, '=');
		if (v) {
			*v++ = 0;
			V = getstr(v);
			rz_sys_setenv(e, V);
			free(V);
		}
	} else if (!strcmp(b, "clearenv")) {
		rz_sys_clearenv();
	}
	if (must_free == true) {
		free(e);
	}
	return true;
}

RZ_API const char *rz_run_help(void) {
	return "program=/bin/ls\n"
	       "arg1=/bin\n"
	       "# arg2=hello\n"
	       "# arg3=\"hello\\nworld\"\n"
	       "# arg4=:048490184058104849\n"
	       "# arg5=:!rz-gg -p n50 -d 10:0x8048123\n"
	       "# arg6=@arg.txt\n"
	       "# arg7=@300@ABCD # 300 chars filled with ABCD pattern\n"
	       "# system=rizin -\n"
	       "# daemon=false\n"
	       "# aslr=no\n"
	       "setenv=FOO=BAR\n"
	       "# unsetenv=FOO\n"
	       "# clearenv=true\n"
	       "# envfile=environ.txt\n"
	       "timeout=3\n"
	       "# timeoutsig=SIGTERM # or 15\n"
	       "# connect=localhost:8080\n"
	       "# listen=8080\n"
	       "# pty=false\n"
	       "# fork=true\n"
	       "# bits=32\n"
	       "# pid=0\n"
	       "# pidfile=/tmp/foo.pid\n"
	       "# #sleep=0\n"
	       "# #maxfd=0\n"
	       "# #execve=false\n"
	       "# #maxproc=0\n"
	       "# #maxstack=0\n"
	       "# #core=false\n"
	       "# #stdio=blah.txt\n"
	       "# #stderr=foo.txt\n"
	       "# stdout=foo.txt\n"
	       "# stdin=input.txt # or !program to redirect input from another program\n"
	       "# input=input.txt\n"
	       "# chdir=/\n"
	       "# chroot=/mnt/chroot\n"
	       "# libpath=$PWD:/tmp/lib\n"
	       "# rzpreload=yes\n"
	       "# preload=/lib/libfoo.so\n"
	       "# setuid=2000\n"
	       "# seteuid=2000\n"
	       "# setgid=2001\n"
	       "# setegid=2001\n"
	       "# nice=5\n";
}

#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
static int fd_forward(int in_fd, int out_fd, char **buff) {
	int size = 0;

	if (ioctl(in_fd, FIONREAD, &size) == -1) {
		perror("ioctl");
		return -1;
	}
	if (!size) { // child process exited or socket is closed
		return -1;
	}

	char *new_buff = realloc(*buff, size);
	if (!new_buff) {
		eprintf("Failed to allocate buffer for redirection");
		return -1;
	}
	*buff = new_buff;
	if (read(in_fd, *buff, size) != size) {
		perror("read");
		return -1;
	}
	if (write(out_fd, *buff, size) != size) {
		perror("write");
		return -1;
	}

	return 0;
}
#endif

static int redirect_socket_to_stdio(RzSocket *sock) {
	close(0);
	close(1);
	close(2);

	dup2(sock->fd, 0);
	dup2(sock->fd, 1);
	dup2(sock->fd, 2);

	return 0;
}

#if __WINDOWS__
static RzThreadFunctionRet exit_process(RzThread *th) {
	// eprintf ("\nrz_run: Interrupted by timeout\n");
	exit(0);
}
#endif

static int redirect_socket_to_pty(RzSocket *sock) {
#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
	// directly duplicating the fds using dup2() creates problems
	// in case of interactive applications
	int fdm, fds;

	if (!dyn_openpty || (dyn_openpty && dyn_openpty(&fdm, &fds, NULL, NULL, NULL) == -1)) {
		perror("opening pty");
		return -1;
	}

	pid_t child_pid = rz_sys_fork();

	if (child_pid == -1) {
		eprintf("cannot fork\n");
		close(fdm);
		close(fds);
		return -1;
	}

	if (child_pid == 0) {
		// child process
		close(fds);

		char *buff = NULL;
		int sockfd = sock->fd;
		int max_fd = fdm > sockfd ? fdm : sockfd;

		while (true) {
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(fdm, &readfds);
			FD_SET(sockfd, &readfds);

			if (select(max_fd + 1, &readfds, NULL, NULL, NULL) == -1) {
				perror("select error");
				break;
			}

			if (FD_ISSET(fdm, &readfds)) {
				if (fd_forward(fdm, sockfd, &buff) != 0) {
					break;
				}
			}

			if (FD_ISSET(sockfd, &readfds)) {
				if (fd_forward(sockfd, fdm, &buff) != 0) {
					break;
				}
			}
		}

		free(buff);
		close(fdm);
		rz_socket_free(sock);
		exit(0);
	}

	// parent
	rz_socket_close_fd(sock);
	if (dyn_login_tty) {
		dyn_login_tty(fds);
	}
	close(fdm);

	// disable the echo on slave stdin
	struct termios t;
	tcgetattr(0, &t);
	cfmakeraw(&t);
	tcsetattr(0, TCSANOW, &t);

	return 0;
#else
	// Fallback to socket to I/O redirection
	return redirect_socket_to_stdio(sock);
#endif
}

RZ_API int rz_run_config_env(RzRunProfile *p) {
	int ret;

#if HAVE_OPENPTY && HAVE_FORKPTY && HAVE_LOGIN_TTY
	dyn_init();
#endif

	if (!p->_program && !p->_system && !p->_runlib) {
		eprintf("No program, system or runlib rule defined\n");
		return 1;
	}
	// when IO is redirected to a process, handle them together
	if (handle_redirection(p->_stdio, true, true, false) != 0) {
		return 1;
	}
	if (handle_redirection(p->_stdin, true, false, false) != 0) {
		return 1;
	}
	if (handle_redirection(p->_stdout, false, true, false) != 0) {
		return 1;
	}
	if (handle_redirection(p->_stderr, false, false, true) != 0) {
		return 1;
	}
	if (p->_aslr != -1) {
		setASLR(p, p->_aslr);
	}
#if __UNIX__
	set_limit(p->_docore, RLIMIT_CORE, RLIM_INFINITY);
	if (p->_maxfd) {
		set_limit(p->_maxfd, RLIMIT_NOFILE, p->_maxfd);
	}
#ifdef RLIMIT_NPROC
	if (p->_maxproc) {
		set_limit(p->_maxproc, RLIMIT_NPROC, p->_maxproc);
	}
#endif
	if (p->_maxstack) {
		set_limit(p->_maxstack, RLIMIT_STACK, p->_maxstack);
	}
#else
	if (p->_docore || p->_maxfd || p->_maxproc || p->_maxstack)
		eprintf("Warning: setrlimits not supported for this platform\n");
#endif
	if (p->_connect) {
		char *q = strchr(p->_connect, ':');
		if (q) {
			RzSocket *fd = rz_socket_new(0);
			*q = 0;
			if (!rz_socket_connect_tcp(fd, p->_connect, q + 1, 30)) {
				eprintf("Cannot connect\n");
				return 1;
			}
			if (p->_pty) {
				if (redirect_socket_to_pty(fd) != 0) {
					eprintf("socket redirection failed\n");
					rz_socket_free(fd);
					return 1;
				}
			} else {
				redirect_socket_to_stdio(fd);
			}
		} else {
			eprintf("Invalid format for connect. missing ':'\n");
			return 1;
		}
	}
	if (p->_listen) {
		RzSocket *child, *fd = rz_socket_new(0);
		bool is_child = false;
		if (!rz_socket_listen(fd, p->_listen, NULL)) {
			eprintf("rz-run: cannot listen\n");
			rz_socket_free(fd);
			return 1;
		}
		while (true) {
			child = rz_socket_accept(fd);
			if (child) {
				is_child = true;

				if (p->_dofork && !p->_dodebug) {
					pid_t child_pid = rz_sys_fork();
					if (child_pid == -1) {
						eprintf("rz-run: cannot fork\n");
						rz_socket_free(child);
						rz_socket_free(fd);
						return 1;
					} else if (child_pid != 0) {
						// parent code
						is_child = false;
					}
				}

				if (is_child) {
					rz_socket_close_fd(fd);
					eprintf("connected\n");
					if (p->_pty) {
						if (redirect_socket_to_pty(child) != 0) {
							eprintf("socket redirection failed\n");
							rz_socket_free(child);
							rz_socket_free(fd);
							return 1;
						}
					} else {
						redirect_socket_to_stdio(child);
					}
					break;
				} else {
					rz_socket_close_fd(child);
				}
			}
		}
		if (!is_child) {
			rz_socket_free(child);
		}
		rz_socket_free(fd);
	}
	if (p->_rzsleep != 0) {
		rz_sys_sleep(p->_rzsleep);
	}
#if __UNIX__
	if (p->_chroot) {
		if (chdir(p->_chroot) == -1) {
			eprintf("Cannot chdir to chroot in %s\n", p->_chroot);
			return 1;
		} else {
			if (chroot(".") == -1) {
				eprintf("Cannot chroot to %s\n", p->_chroot);
				return 1;
			} else {
				// Silenting pedantic meson flags...
				if (chdir("/") == -1) {
					eprintf("Cannot chdir to /\n");
					return 1;
				}
				if (p->_chgdir) {
					if (chdir(p->_chgdir) == -1) {
						eprintf("Cannot chdir after chroot to %s\n", p->_chgdir);
						return 1;
					}
				}
			}
		}
	} else if (p->_chgdir) {
		if (chdir(p->_chgdir) == -1) {
			eprintf("Cannot chdir after chroot to %s\n", p->_chgdir);
			return 1;
		}
	}
#else
	if (p->_chgdir) {
		ret = chdir(p->_chgdir);
		if (ret < 0) {
			return 1;
		}
	}
	if (p->_chroot) {
		ret = chdir(p->_chroot);
		if (ret < 0) {
			return 1;
		}
	}
#endif
#if __UNIX__
	if (p->_setuid) {
		ret = setgroups(0, NULL);
		if (ret < 0) {
			return 1;
		}
		ret = setuid(atoi(p->_setuid));
		if (ret < 0) {
			return 1;
		}
	}
	if (p->_seteuid) {
		ret = seteuid(atoi(p->_seteuid));
		if (ret < 0) {
			return 1;
		}
	}
	if (p->_setgid) {
		ret = setgid(atoi(p->_setgid));
		if (ret < 0) {
			return 1;
		}
	}
	if (p->_input) {
		char *inp;
		int f2[2];
		if (rz_sys_pipe(f2, true) != -1) {
			close(0);
			dup2(f2[0], 0);
		} else {
			eprintf("[ERROR] rz-run: Cannot create pipe\n");
			return 1;
		}
		inp = getstr(p->_input);
		if (inp) {
			size_t inpl = strlen(inp);
			if (write(f2[1], inp, inpl) != inpl) {
				eprintf("[ERROR] rz-run: Cannot write to the pipe\n");
			}
			rz_sys_pipe_close(f2[1]);
			free(inp);
		} else {
			eprintf("Invalid input\n");
		}
	}
#endif
	if (p->_rzpreload) {
		if (p->_preload) {
			eprintf("WARNING: Only one library can be opened at a time\n");
		}
		char *libdir = rz_path_libdir();
		p->_preload = rz_file_path_join(libdir, "librz." RZ_LIB_EXT);
		free(libdir);
	}
	if (p->_libpath) {
#if __WINDOWS__
		eprintf("rz-run: libpath unsupported for this platform\n");
#elif __HAIKU__
		char *orig = rz_sys_getenv("LIBRARY_PATH");
		char *newlib = rz_str_newf("%s:%s", p->_libpath, orig);
		rz_sys_setenv("LIBRARY_PATH", newlib);
		free(newlib);
		free(orig);
#elif __APPLE__
		rz_sys_setenv("DYLD_LIBRARY_PATH", p->_libpath);
#else
		rz_sys_setenv("LD_LIBRARY_PATH", p->_libpath);
#endif
	}
	if (p->_preload) {
#if __APPLE__
		// 10.6
#ifndef __MAC_10_7
		rz_sys_setenv("DYLD_PRELOAD", p->_preload);
#endif
		rz_sys_setenv("DYLD_INSERT_LIBRARIES", p->_preload);
		// 10.8
		rz_sys_setenv("DYLD_FORCE_FLAT_NAMESPACE", "1");
#else
		rz_sys_setenv("LD_PRELOAD", p->_preload);
#endif
	}
	if (p->_timeout) {
#if __UNIX__
		int mypid = getpid();
		if (!rz_sys_fork()) {
			int use_signal = p->_timeout_sig;
			if (use_signal < 1) {
				use_signal = SIGKILL;
			}
			sleep(p->_timeout);
			if (!kill(mypid, 0)) {
				// eprintf ("\nrz_run: Interrupted by timeout\n");
			}
			kill(mypid, use_signal);
			exit(0);
		}
#else
		if (p->_timeout_sig < 1 || p->_timeout_sig == 9) {
			rz_th_new(exit_process, NULL, p->_timeout);
		} else {
			eprintf("timeout with signal not supported for this platform\n");
		}
#endif
	}
	return 0;
}

// NOTE: return value is like in unix return code (0 = ok, 1 = not ok)
RZ_API int rz_run_start(RzRunProfile *p) {
#if HAVE_EXECVE
	if (p->_execve) {
		exit(rz_sys_execv(p->_program, (char *const *)p->_args));
	}
#endif
#if __APPLE__ && HAVE_FORK
	posix_spawnattr_t attr = { 0 };
	pid_t pid = -1;
	int ret;
	posix_spawnattr_init(&attr);
	if (p->_args[0]) {
		char **envp = rz_sys_get_environ();
		ut32 spflags = 0; // POSIX_SPAWN_START_SUSPENDED;
		spflags |= POSIX_SPAWN_SETEXEC;
		if (p->_aslr == 0) {
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
			spflags |= _POSIX_SPAWN_DISABLE_ASLR;
		}
		(void)posix_spawnattr_setflags(&attr, spflags);
		if (p->_bits) {
			size_t copied = 1;
			cpu_type_t cpu;
#if __i386__ || __x86_64__
			cpu = CPU_TYPE_I386;
			if (p->_bits == 64) {
				cpu |= CPU_ARCH_ABI64;
			}
#else
			cpu = CPU_TYPE_ANY;
#endif
			posix_spawnattr_setbinpref_np(
				&attr, 1, &cpu, &copied);
		}
		ret = posix_spawnp(&pid, p->_args[0],
			NULL, &attr, p->_args, envp);
		switch (ret) {
		case 0:
			break;
		default:
			eprintf("posix_spawnp: %s\n", strerror(ret));
			break;
		}
		exit(ret);
	}
#endif
	if (p->_system) {
		if (p->_pid) {
			eprintf("PID: Cannot determine pid with 'system' directive. Use 'program'.\n");
		}
		if (p->_daemon) {
#if __WINDOWS__
			//		eprintf ("PID: Cannot determine pid with 'system' directive. Use 'program'.\n");
#else
			pid_t child = rz_sys_fork();
			if (child == -1) {
				perror("fork");
				exit(1);
			}
			if (child) {
				if (p->_pidfile) {
					char pidstr[32];
					snprintf(pidstr, sizeof(pidstr), "%d\n", child);
					rz_file_dump(p->_pidfile,
						(const ut8 *)pidstr,
						strlen(pidstr), 0);
				}
				exit(0);
			}
			setsid();
			if (p->_timeout) {
#if __UNIX__
				int mypid = getpid();
				if (!rz_sys_fork()) {
					int use_signal = p->_timeout_sig;
					if (use_signal < 1) {
						use_signal = SIGKILL;
					}
					sleep(p->_timeout);
					if (!kill(mypid, 0)) {
						// eprintf ("\nrz_run: Interrupted by timeout\n");
					}
					kill(mypid, use_signal);
					exit(0);
				}
#else
				eprintf("timeout not supported for this platform\n");
#endif
			}
#endif
#if __UNIX__
			close(0);
			close(1);
			char *bin_sh = rz_file_binsh();
			int ret = rz_sys_execl(bin_sh, "sh", "-c", p->_system, NULL);
			free(bin_sh);
			exit(ret);
#else
			exit(rz_sys_system(p->_system));
#endif
		} else {
			if (p->_pidfile) {
				eprintf("Warning: pidfile doesnt work with 'system'.\n");
			}
			exit(rz_sys_system(p->_system));
		}
	}
	if (p->_program) {
		if (!rz_file_exists(p->_program)) {
			char *progpath = rz_file_path(p->_program);
			if (progpath && *progpath) {
				free(p->_program);
				p->_program = progpath;
			} else {
				free(progpath);
				eprintf("rz-run: %s: file not found\n", p->_program);
				return 1;
			}
		}
#if __UNIX__
		// XXX HACK close all non-tty fds
		{
			int i;
			for (i = 3; i < 1024; i++) {
				close(i);
			}
		}
		// TODO: use posix_spawn
		if (p->_setgid) {
			int ret = setgid(atoi(p->_setgid));
			if (ret < 0) {
				return 1;
			}
		}
		if (p->_pid) {
			eprintf("PID: %d\n", getpid());
		}
		if (p->_pidfile) {
			char pidstr[32];
			snprintf(pidstr, sizeof(pidstr), "%d\n", getpid());
			rz_file_dump(p->_pidfile,
				(const ut8 *)pidstr,
				strlen(pidstr), 0);
		}
#endif

		if (p->_nice) {
#if HAVE_NICE
			if (nice(p->_nice) == -1) {
				return 1;
			}
#else
			eprintf("nice not supported for this platform\n");
#endif
		}
		if (p->_daemon) {
#if __WINDOWS__
			eprintf("PID: Cannot determine pid with 'system' directive. Use 'program'.\n");
#else
			pid_t child = rz_sys_fork();
			if (child == -1) {
				perror("fork");
				exit(1);
			}
			if (child) {
				if (p->_pidfile) {
					char pidstr[32];
					snprintf(pidstr, sizeof(pidstr), "%d\n", child);
					rz_file_dump(p->_pidfile,
						(const ut8 *)pidstr,
						strlen(pidstr), 0);
					exit(0);
				}
			}
			setsid();
#if HAVE_EXECVE
			exit(rz_sys_execv(p->_program, (char *const *)p->_args));
#endif
#endif
		}
#if HAVE_EXECVE
		exit(rz_sys_execv(p->_program, (char *const *)p->_args));
#endif
	}
	if (p->_runlib) {
		if (!p->_runlib_fcn) {
			eprintf("No function specified. Please set runlib.fcn\n");
			return 1;
		}
		void *addr = rz_lib_dl_open(p->_runlib);
		if (!addr) {
			eprintf("Could not load the library '%s'\n", p->_runlib);
			return 1;
		}
		void (*fcn)(void) = rz_lib_dl_sym(addr, p->_runlib_fcn);
		if (!fcn) {
			eprintf("Could not find the function '%s'\n", p->_runlib_fcn);
			return 1;
		}
		switch (p->_argc) {
		case 0:
			fcn();
			break;
		case 1:
			rz_run_call1(fcn, p->_args[1]);
			break;
		case 2:
			rz_run_call2(fcn, p->_args[1], p->_args[2]);
			break;
		case 3:
			rz_run_call3(fcn, p->_args[1], p->_args[2], p->_args[3]);
			break;
		case 4:
			rz_run_call4(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4]);
			break;
		case 5:
			rz_run_call5(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5]);
			break;
		case 6:
			rz_run_call6(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5], p->_args[6]);
			break;
		case 7:
			rz_run_call7(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5], p->_args[6], p->_args[7]);
			break;
		case 8:
			rz_run_call8(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5], p->_args[6], p->_args[7], p->_args[8]);
			break;
		case 9:
			rz_run_call9(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5], p->_args[6], p->_args[7], p->_args[8], p->_args[9]);
			break;
		case 10:
			rz_run_call10(fcn, p->_args[1], p->_args[2], p->_args[3], p->_args[4],
				p->_args[5], p->_args[6], p->_args[7], p->_args[8], p->_args[9], p->_args[10]);
			break;
		default:
			eprintf("Too many arguments.\n");
			return 1;
		}
		rz_lib_dl_close(addr);
	}
	return 0;
}

RZ_API char *rz_run_get_environ_profile(char **env) {
	if (!env) {
		return NULL;
	}
	RzStrBuf *sb = rz_strbuf_new(NULL);
	while (*env) {
		char *k = strdup(*env);
		char *v = strchr(k, '=');
		if (v) {
			*v++ = 0;
			RzStrEscOptions opt = { 0 };
			opt.show_asciidot = false;
			opt.esc_bslash = true;
			v = rz_str_escape_8bit(v, true, &opt);
			if (v) {
				rz_strbuf_appendf(sb, "setenv=%s=\"%s\"\n", k, v);
				free(v);
			}
		}
		free(k);
		env++;
	}
	return rz_strbuf_drain(sb);
}
