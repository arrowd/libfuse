/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2005-2008 Csaba Henk <csaba.henk@creo.hu>

  Architecture specific file system mounting (FreeBSD).

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#include "fuse_config.h"
#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_opt.h"
#include "util.h"

#include <sys/param.h>
#include "fuse_mount_compat.h"

#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/user.h>
#include <sys/sysctl.h>

#define FUSERMOUNT_PROG		"mount_fusefs"
#define FUSE_DEV_TRUNK		"/dev/fuse"

enum {
	KEY_RO,
	KEY_KERN
};

struct mount_opts {
	int allow_other;
	char *kernel_opts;
	unsigned max_read;
	int auto_unmount;
};

#define FUSE_DUAL_OPT_KEY(templ, key)				\
	FUSE_OPT_KEY(templ, key), FUSE_OPT_KEY("no" templ, key)

static const struct fuse_opt fuse_mount_opts[] = {
	{ "allow_other", offsetof(struct mount_opts, allow_other), 1 },
	{ "max_read=%u", offsetof(struct mount_opts, max_read), 1 },
	{ "auto_unmount", offsetof(struct mount_opts, auto_unmount), 1 },
	FUSE_OPT_KEY("-r",			KEY_RO),
	/* standard FreeBSD mount options */
	FUSE_DUAL_OPT_KEY("dev",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("async",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("atime",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("dev",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("exec",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("suid",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("symfollow",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("rdonly",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("sync",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("union",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("userquota",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("groupquota",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("clusterr",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("clusterw",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("suiddir",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("snapshot",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("multilabel",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("acls",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("force",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("update",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("ro",			KEY_KERN),
	FUSE_DUAL_OPT_KEY("rw",			KEY_KERN),
	FUSE_DUAL_OPT_KEY("auto",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("automounted",	KEY_KERN),
	/* options supported under both Linux and FBSD */
	FUSE_DUAL_OPT_KEY("allow_other",	KEY_KERN),
	FUSE_DUAL_OPT_KEY("default_permissions",KEY_KERN),
	FUSE_OPT_KEY("max_read=",		KEY_KERN),
	FUSE_OPT_KEY("subtype=",		KEY_KERN),
	/* FBSD FUSE specific mount options */
	FUSE_DUAL_OPT_KEY("private",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("neglect_shares",	KEY_KERN),
	FUSE_DUAL_OPT_KEY("push_symlinks_in",	KEY_KERN),
#if __FreeBSD_version >= 1200519
	FUSE_DUAL_OPT_KEY("intr",		KEY_KERN),
#endif
	/* stock FBSD mountopt parsing routine lets anything be negated... */
	/*
	 * Linux specific mount options, but let just the mount util
	 * handle them
	 */
	FUSE_OPT_KEY("fsname=",			KEY_KERN),
	FUSE_OPT_END
};

void fuse_mount_version(void)
{
	system(FUSERMOUNT_PROG " --version");
}

unsigned get_max_read(struct mount_opts *o)
{
	return o->max_read;
}

static int resolve_pid(pid_t pid, struct kinfo_proc *out)
{
	int mib[4];
	size_t size = sizeof(struct kinfo_proc);

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = pid;
	return sysctl(mib, 4, out, &size, NULL, 0);
}

static void start_auto_unmounter(const char *mountpoint,
			       struct kinfo_proc *client_kproc)
{
	const char *mountprog = "/usr/obj/usr/src/amd64.amd64/sbin/mount_fusefs/mount_fusefs";
	int pid = rfork(RFCFDG | RFPROC | RFNOWAIT);

	if (pid == -1) {
		perror("fuse: rfork() failed");
		return;
	}

	if (pid == 0) {
		struct statfs stat_buf;
		char auto_unmount_arg[128];
		const char *argv[32];
		int a = 0;
		close(0);

		if (statfs(mountpoint, &stat_buf) != 0) {
			/* It is unlikely that we get there, but if we do, what should we do?
			 * Unmount the FS, because we can't fulfill the auto-unmount promise,
			 * or let the caller proceed?
			 */
			perror("fuse: failed to setup auto-unmount due to statfs error");
			_exit(EXIT_FAILURE);
		}
		snprintf(auto_unmount_arg, sizeof(auto_unmount_arg), "%d,%ld,%ld,%d,%d",
			client_kproc->ki_pid, client_kproc->ki_start.tv_sec,
			client_kproc->ki_start.tv_usec, stat_buf.f_fsid.val[0], stat_buf.f_fsid.val[1]);

		argv[a++] = mountprog;
		argv[a++] = "--auto-unmount";
		argv[a++] = auto_unmount_arg;
		argv[a++] = "whatever";
		argv[a++] = mountpoint;
		argv[a++] = NULL;
		execvp(mountprog, (char **) argv);
		perror("fuse: failed to exec auto-unmount program");
		_exit(EXIT_FAILURE);
	}
}

static int fuse_mount_opt_proc(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	(void) outargs;
	struct mount_opts *mo = data;

	switch (key) {
	case KEY_RO:
		arg = "ro";
		/* fall through */

	case KEY_KERN:
		return fuse_opt_add_opt(&mo->kernel_opts, arg);
	}

	/* Pass through unknown options */
	return 1;
}

void fuse_kern_unmount(const char *mountpoint, int fd)
{
	if (close(fd) < 0)
		fuse_log(FUSE_LOG_ERR, "closing FD %d failed: %s", fd, strerror(errno));
	if (unmount(mountpoint, MNT_FORCE) < 0)
		fuse_log(FUSE_LOG_ERR, "unmounting %s failed: %s",
			mountpoint, strerror(errno));
}

static int fuse_mount_core(const char *mountpoint, const char *opts, int auto_unmount)
{
	const char *mountprog = FUSERMOUNT_PROG;
	struct kinfo_proc client_kproc;
	long fd;
	char *fdnam, *dev;
	pid_t pid, cpid;
	int status;
	int err;

	fdnam = getenv("FUSE_DEV_FD");

	if (fdnam) {
		err = libfuse_strtol(fdnam, &fd);
		if (err || fd < 0) {
			fuse_log(FUSE_LOG_ERR, "invalid value given in FUSE_DEV_FD\n");
			return -1;
		}

		goto mount;
	}

	dev = getenv("FUSE_DEV_NAME");

	if (! dev)
		dev = (char *)FUSE_DEV_TRUNK;

	if ((fd = open(dev, O_RDWR)) < 0) {
		perror("fuse: failed to open fuse device");
		return -1;
	}

mount:
	if (getenv("FUSE_NO_MOUNT") || ! mountpoint)
		goto out;

	if (resolve_pid(getpid(), &client_kproc)) {
		perror("fuse: failed to lookup process information");
		close(fd);
		return -1;
	}

	pid = fork();
	cpid = pid;

	if (pid == -1) {
		perror("fuse: fork() failed");
		close(fd);
		return -1;
	}

	if (pid == 0) {
		pid = fork();

		if (pid == -1) {
			perror("fuse: fork() failed");
			close(fd);
			_exit(EXIT_FAILURE);
		}

		if (pid == 0) {
			const char *argv[32];
			int a = 0;
			int ret = -1;

			if (! fdnam)
			{
				ret = asprintf(&fdnam, "%ld", fd);
				if(ret == -1)
				{
					perror("fuse: failed to assemble mount arguments");
					close(fd);
					_exit(EXIT_FAILURE);
				}
			}

			argv[a++] = mountprog;
			if (opts && !auto_unmount) {
				argv[a++] = "-o";
				argv[a++] = opts;
			} else if (!opts && auto_unmount) {
				/* if client wants auto_unmount we pass nocover to help
				 * mount_fusefs fulfill its unmounting promise
				 */
				argv[a++] = "-o";
				argv[a++] = "nocover";
			} else if (opts && auto_unmount) {
				static const char nocover_opt[] = ",nocover";
				int optslen = strlen(opts);
				argv[a++] = "-o";
				argv[a++] = malloc(optslen + sizeof(nocover_opt) + 1);
				strcpy(__DECONST(char*, argv[a-1]), opts);
				strcpy(__DECONST(char*, argv[a-1] + optslen), nocover_opt);
			}
			argv[a++] = fdnam;
			argv[a++] = mountpoint;
			argv[a++] = NULL;
			execvp(mountprog, (char **) argv);
			perror("fuse: failed to exec mount program");
			free(fdnam);
			_exit(EXIT_FAILURE);
		}

		waitpid(pid, &status, 0);
		if (!WIFEXITED(status))
			_exit(EXIT_FAILURE);

		if (auto_unmount)
			start_auto_unmounter(mountpoint, &client_kproc);

		_exit(WEXITSTATUS(status));
	}

	if (waitpid(cpid, &status, 0) == -1 || WEXITSTATUS(status) != 0) {
		perror("fuse: failed to mount file system");
		if (close(fd) < 0)
			perror("fuse: closing FD");
		return -1;
	}

out:
	return fd;
}

struct mount_opts *parse_mount_opts(struct fuse_args *args)
{
	struct mount_opts *mo;

	mo = (struct mount_opts*) malloc(sizeof(struct mount_opts));
	if (mo == NULL)
		return NULL;

	memset(mo, 0, sizeof(struct mount_opts));

	if (args &&
	    fuse_opt_parse(args, mo, fuse_mount_opts, fuse_mount_opt_proc) == -1)
		goto err_out;

	return mo;

err_out:
	destroy_mount_opts(mo);
	return NULL;
}

void destroy_mount_opts(struct mount_opts *mo)
{
	free(mo->kernel_opts);
	free(mo);
}

int fuse_kern_mount(const char *mountpoint, struct mount_opts *mo)
{
	/* mount util should not try to spawn the daemon */
	setenv("MOUNT_FUSEFS_SAFE", "1", 1);
	/* to notify the mount util it's called from lib */
	setenv("MOUNT_FUSEFS_CALL_BY_LIB", "1", 1);

	return fuse_mount_core(mountpoint, mo->kernel_opts, mo->auto_unmount);
}
