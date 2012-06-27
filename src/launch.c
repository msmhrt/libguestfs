/* libguestfs
 * Copyright (C) 2009-2012 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#define _BSD_SOURCE /* for mkdtemp, usleep */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <assert.h>

#include <rpc/types.h>
#include <rpc/xdr.h>

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>

#include "c-ctype.h"
#include "ignore-value.h"
#include "glthread/lock.h"

#include "guestfs.h"
#include "guestfs-internal.h"
#include "guestfs-internal-actions.h"
#include "guestfs_protocol.h"

#define NETWORK "10.0.2.0/24"
#define ROUTER "10.0.2.2"

static int launch_appliance (guestfs_h *g);
static int64_t timeval_diff (const struct timeval *x, const struct timeval *y);
static void print_qemu_command_line (guestfs_h *g, char **argv);
static int connect_unix_socket (guestfs_h *g, const char *sock);
static int check_peer_euid (guestfs_h *g, int sock, uid_t *rtn);
static int qemu_supports (guestfs_h *g, const char *option);
static int qemu_supports_device (guestfs_h *g, const char *device_name);
static int qemu_supports_virtio_scsi (guestfs_h *g);
static char *qemu_drive_param (guestfs_h *g, const struct drive *drv, size_t index);
static char *drive_name (size_t index, char *ret);

#if 0
static int qemu_supports_re (guestfs_h *g, const pcre *option_regex);

static void compile_regexps (void) __attribute__((constructor));
static void free_regexps (void) __attribute__((destructor));

static void
compile_regexps (void)
{
  const char *err;
  int offset;

#define COMPILE(re,pattern,options)                                     \
  do {                                                                  \
    re = pcre_compile ((pattern), (options), &err, &offset, NULL);      \
    if (re == NULL) {                                                   \
      ignore_value (write (2, err, strlen (err)));                      \
      abort ();                                                         \
    }                                                                   \
  } while (0)
}

static void
free_regexps (void)
{
}
#endif

/* Functions to add a string to the current command line. */
static void
alloc_cmdline (guestfs_h *g)
{
  if (g->cmdline == NULL) {
    /* g->cmdline[0] is reserved for argv[0], set in guestfs_launch. */
    g->cmdline_size = 1;
    g->cmdline = safe_malloc (g, sizeof (char *));
    g->cmdline[0] = NULL;
  }
}

static void
incr_cmdline_size (guestfs_h *g)
{
  alloc_cmdline (g);
  g->cmdline_size++;
  g->cmdline = safe_realloc (g, g->cmdline, sizeof (char *) * g->cmdline_size);
}

static int
add_cmdline (guestfs_h *g, const char *str)
{
  if (g->state != CONFIG) {
    error (g,
        _("command line cannot be altered after qemu subprocess launched"));
    return -1;
  }

  incr_cmdline_size (g);
  g->cmdline[g->cmdline_size-1] = safe_strdup (g, str);
  return 0;
}

/* Like 'add_cmdline' but allowing a shell-quoted string of zero or
 * more options.  XXX The unquoting is not very clever.
 */
static int
add_cmdline_shell_unquoted (guestfs_h *g, const char *options)
{
  char quote;
  const char *startp, *endp, *nextp;

  if (g->state != CONFIG) {
    error (g,
        _("command line cannot be altered after qemu subprocess launched"));
    return -1;
  }

  while (*options) {
    quote = *options;
    if (quote == '\'' || quote == '"')
      startp = options+1;
    else {
      startp = options;
      quote = ' ';
    }

    endp = strchr (options, quote);
    if (endp == NULL) {
      if (quote != ' ') {
        error (g, _("unclosed quote character (%c) in command line near: %s"),
               quote, options);
        return -1;
      }
      endp = options + strlen (options);
    }

    if (quote == ' ')
      nextp = endp+1;
    else {
      if (!endp[1])
        nextp = endp+1;
      else if (endp[1] == ' ')
        nextp = endp+2;
      else {
        error (g, _("cannot parse quoted string near: %s"), options);
        return -1;
      }
    }
    while (*nextp && *nextp == ' ')
      nextp++;

    incr_cmdline_size (g);
    g->cmdline[g->cmdline_size-1] = safe_strndup (g, startp, endp-startp);

    options = nextp;
  }

  return 0;
}

struct drive **
guestfs___checkpoint_drives (guestfs_h *g)
{
  struct drive **i = &g->drives;
  while (*i != NULL) i = &((*i)->next);
  return i;
}

void
guestfs___rollback_drives (guestfs_h *g, struct drive **i)
{
  guestfs___free_drives(i);
}

/* Internal command to return the command line. */
char **
guestfs__debug_cmdline (guestfs_h *g)
{
  size_t i;
  char **r;

  alloc_cmdline (g);

  r = safe_malloc (g, sizeof (char *) * (g->cmdline_size + 1));
  r[0] = safe_strdup (g, g->qemu); /* g->cmdline[0] is always NULL */

  for (i = 1; i < g->cmdline_size; ++i)
    r[i] = safe_strdup (g, g->cmdline[i]);

  r[g->cmdline_size] = NULL;

  return r;                     /* caller frees */
}

/* Internal command to return the list of drives. */
char **
guestfs__debug_drives (guestfs_h *g)
{
  size_t i, count;
  char **ret;
  struct drive *drv;

  for (count = 0, drv = g->drives; drv; count++, drv = drv->next)
    ;

  ret = safe_malloc (g, sizeof (char *) * (count + 1));

  for (i = 0, drv = g->drives; drv; i++, drv = drv->next)
    ret[i] = qemu_drive_param (g, drv, i);

  ret[count] = NULL;

  return ret;                   /* caller frees */
}

int
guestfs__config (guestfs_h *g,
                 const char *qemu_param, const char *qemu_value)
{
  if (qemu_param[0] != '-') {
    error (g, _("guestfs_config: parameter must begin with '-' character"));
    return -1;
  }

  /* A bit fascist, but the user will probably break the extra
   * parameters that we add if they try to set any of these.
   */
  if (STREQ (qemu_param, "-kernel") ||
      STREQ (qemu_param, "-initrd") ||
      STREQ (qemu_param, "-nographic") ||
      STREQ (qemu_param, "-serial") ||
      STREQ (qemu_param, "-full-screen") ||
      STREQ (qemu_param, "-std-vga") ||
      STREQ (qemu_param, "-vnc")) {
    error (g, _("guestfs_config: parameter '%s' isn't allowed"), qemu_param);
    return -1;
  }

  if (add_cmdline (g, qemu_param) != 0) return -1;

  if (qemu_value != NULL) {
    if (add_cmdline (g, qemu_value) != 0) return -1;
  }

  return 0;
}

/* cache=off improves reliability in the event of a host crash.
 *
 * However this option causes qemu to try to open the file with
 * O_DIRECT.  This fails on some filesystem types (notably tmpfs).
 * So we check if we can open the file with or without O_DIRECT,
 * and use cache=off (or not) accordingly.
 *
 * NB: This function is only called on the !readonly path.  We must
 * try to open with O_RDWR to test that the file is readable and
 * writable here.
 */
static int
test_cache_off (guestfs_h *g, const char *filename)
{
  int fd = open (filename, O_RDWR|O_DIRECT);
  if (fd >= 0) {
    close (fd);
    return 1;
  }

  fd = open (filename, O_RDWR);
  if (fd >= 0) {
    close (fd);
    return 0;
  }

  perrorf (g, "%s", filename);
  return -1;
}

/* Check string parameter matches ^[-_[:alnum:]]+$ (in C locale). */
static int
valid_format_iface (const char *str)
{
  size_t len = strlen (str);

  if (len == 0)
    return 0;

  while (len > 0) {
    char c = *str++;
    len--;
    if (c != '-' && c != '_' && !c_isalnum (c))
      return 0;
  }
  return 1;
}

int
guestfs__add_drive_opts (guestfs_h *g, const char *filename,
                         const struct guestfs_add_drive_opts_argv *optargs)
{
  int readonly;
  char *format;
  char *iface;
  char *name;
  int use_cache_off;
  int is_null;

  if (strchr (filename, ':') != NULL) {
    error (g, _("filename cannot contain ':' (colon) character. "
                "This is a limitation of qemu."));
    return -1;
  }

  readonly = optargs->bitmask & GUESTFS_ADD_DRIVE_OPTS_READONLY_BITMASK
             ? optargs->readonly : 0;
  format = optargs->bitmask & GUESTFS_ADD_DRIVE_OPTS_FORMAT_BITMASK
           ? safe_strdup (g, optargs->format) : NULL;
  iface = optargs->bitmask & GUESTFS_ADD_DRIVE_OPTS_IFACE_BITMASK
          ? safe_strdup (g, optargs->iface) : NULL;
  name = optargs->bitmask & GUESTFS_ADD_DRIVE_OPTS_NAME_BITMASK
          ? safe_strdup (g, optargs->name) : NULL;

  if (format && !valid_format_iface (format)) {
    error (g, _("%s parameter is empty or contains disallowed characters"),
           "format");
    goto err_out;
  }
  if (iface && !valid_format_iface (iface)) {
    error (g, _("%s parameter is empty or contains disallowed characters"),
           "iface");
    goto err_out;
  }

  /* Traditionally you have been able to use /dev/null as a filename,
   * as many times as you like.  Treat this as a special case, because
   * old versions of qemu have some problems.
   */
  is_null = STREQ (filename, "/dev/null");
  if (is_null) {
    if (format && STRNEQ (format, "raw")) {
      error (g, _("for device '/dev/null', format must be 'raw'"));
      goto err_out;
    }
    /* Ancient KVM (RHEL 5) cannot handle the case where we try to add
     * a snapshot on top of /dev/null.  Modern qemu can handle it OK,
     * but the device size is still 0, so it shouldn't matter whether
     * or not this is readonly.
     */
    readonly = 0;
  }

  /* For writable files, see if we can use cache=off.  This also
   * checks for the existence of the file.  For readonly we have
   * to do the check explicitly.
   */
  use_cache_off = readonly ? 0 : test_cache_off (g, filename);
  if (use_cache_off == -1)
    goto err_out;

  if (readonly) {
    if (access (filename, R_OK) == -1) {
      perrorf (g, "%s", filename);
      goto err_out;
    }
  }

  struct drive **i = &(g->drives);
  while (*i != NULL) i = &((*i)->next);

  *i = safe_malloc (g, sizeof (struct drive));
  (*i)->next = NULL;
  (*i)->path = safe_strdup (g, filename);
  (*i)->readonly = readonly;
  (*i)->format = format;
  (*i)->iface = iface;
  (*i)->name = name;
  (*i)->use_cache_off = use_cache_off;

  return 0;

err_out:
  free (format);
  free (iface);
  free (name);
  return -1;
}

int
guestfs__add_drive (guestfs_h *g, const char *filename)
{
  struct guestfs_add_drive_opts_argv optargs = {
    .bitmask = 0,
  };

  return guestfs__add_drive_opts (g, filename, &optargs);
}

int
guestfs__add_drive_ro (guestfs_h *g, const char *filename)
{
  struct guestfs_add_drive_opts_argv optargs = {
    .bitmask = GUESTFS_ADD_DRIVE_OPTS_READONLY_BITMASK,
    .readonly = 1,
  };

  return guestfs__add_drive_opts (g, filename, &optargs);
}

int
guestfs__add_drive_with_if (guestfs_h *g, const char *filename,
                            const char *iface)
{
  struct guestfs_add_drive_opts_argv optargs = {
    .bitmask = GUESTFS_ADD_DRIVE_OPTS_IFACE_BITMASK,
    .iface = iface,
  };

  return guestfs__add_drive_opts (g, filename, &optargs);
}

int
guestfs__add_drive_ro_with_if (guestfs_h *g, const char *filename,
                               const char *iface)
{
  struct guestfs_add_drive_opts_argv optargs = {
    .bitmask = GUESTFS_ADD_DRIVE_OPTS_IFACE_BITMASK
             | GUESTFS_ADD_DRIVE_OPTS_READONLY_BITMASK,
    .iface = iface,
    .readonly = 1,
  };

  return guestfs__add_drive_opts (g, filename, &optargs);
}

int
guestfs__add_cdrom (guestfs_h *g, const char *filename)
{
  if (strchr (filename, ':') != NULL) {
    error (g, _("filename cannot contain ':' (colon) character. "
                "This is a limitation of qemu."));
    return -1;
  }

  if (access (filename, F_OK) == -1) {
    perrorf (g, "%s", filename);
    return -1;
  }

  return guestfs__config (g, "-cdrom", filename);
}

static int is_openable (guestfs_h *g, const char *path, int flags);

int
guestfs__launch (guestfs_h *g)
{
  /* Configured? */
  if (g->state != CONFIG) {
    error (g, _("the libguestfs handle has already been launched"));
    return -1;
  }

  TRACE0 (launch_start);

  /* Make the temporary directory. */
  if (!g->tmpdir) {
    TMP_TEMPLATE_ON_STACK (dir_template);
    g->tmpdir = safe_strdup (g, dir_template);
    if (mkdtemp (g->tmpdir) == NULL) {
      perrorf (g, _("%s: cannot create temporary directory"), dir_template);
      return -1;
    }
  }

  /* Allow anyone to read the temporary directory.  The socket in this
   * directory won't be readable but anyone can see it exists if they
   * want. (RHBZ#610880).
   */
  if (chmod (g->tmpdir, 0755) == -1)
    warning (g, "chmod: %s: %m (ignored)", g->tmpdir);

  /* Launch the appliance or attach to an existing daemon. */
  switch (g->attach_method) {
  case ATTACH_METHOD_APPLIANCE:
    return launch_appliance (g);

  case ATTACH_METHOD_UNIX:
    return connect_unix_socket (g, g->attach_method_arg);

  default:
    abort ();
  }
}

/* RHBZ#790721: It makes no sense to have multiple threads racing to
 * build the appliance from within a single process, and the code
 * isn't safe for that anyway.  Therefore put a thread lock around
 * appliance building.
 */
gl_lock_define_initialized (static, building_lock);

static int
launch_appliance (guestfs_h *g)
{
  int r;
  int wfd[2], rfd[2];
  char guestfsd_sock[256];
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof addr;
  int null_vmchannel_port;

  /* At present you must add drives before starting the appliance.  In
   * future when we enable hotplugging you won't need to do this.
   */
  if (!g->drives) {
    error (g, _("you must call guestfs_add_drive before guestfs_launch"));
    return -1;
  }

  /* Start the clock ... */
  gettimeofday (&g->launch_t, NULL);
  guestfs___launch_send_progress (g, 0);

  TRACE0 (launch_build_appliance_start);

  /* Locate and/or build the appliance. */
  char *kernel = NULL, *initrd = NULL, *appliance = NULL;
  gl_lock_lock (building_lock);
  if (guestfs___build_appliance (g, &kernel, &initrd, &appliance) == -1) {
    gl_lock_unlock (building_lock);
    return -1;
  }
  gl_lock_unlock (building_lock);

  TRACE0 (launch_build_appliance_end);

  guestfs___launch_send_progress (g, 3);

  if (g->verbose)
    guestfs___print_timestamped_message (g, "begin testing qemu features");

  /* Get qemu help text and version. */
  if (qemu_supports (g, NULL) == -1)
    goto cleanup0;

  /* "Null vmchannel" implementation: We allocate a random port
   * number on the host, and the daemon connects back to it.  To
   * make this secure, we check that the peer UID is the same as our
   * UID.  This requires SLIRP (user mode networking in qemu).
   */
  g->sock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g->sock == -1) {
    perrorf (g, "socket");
    goto cleanup0;
  }

  addr.sin_family = AF_INET;
  addr.sin_port = htons (0);
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  if (bind (g->sock, (struct sockaddr *) &addr, addrlen) == -1) {
    perrorf (g, "bind");
    goto cleanup0;
  }

  if (listen (g->sock, 256) == -1) {
    perrorf (g, "listen");
    goto cleanup0;
  }

  if (getsockname (g->sock, (struct sockaddr *) &addr, &addrlen) == -1) {
    perrorf (g, "getsockname");
    goto cleanup0;
  }

  if (fcntl (g->sock, F_SETFL, O_NONBLOCK) == -1) {
    perrorf (g, "fcntl");
    goto cleanup0;
  }

  null_vmchannel_port = ntohs (addr.sin_port);
  debug (g, "null_vmchannel_port = %d", null_vmchannel_port);

  if (!g->direct) {
    if (pipe (wfd) == -1 || pipe (rfd) == -1) {
      perrorf (g, "pipe");
      goto cleanup0;
    }
  }

  if (g->verbose)
    guestfs___print_timestamped_message (g, "finished testing qemu features");

  r = fork ();
  if (r == -1) {
    perrorf (g, "fork");
    if (!g->direct) {
      close (wfd[0]);
      close (wfd[1]);
      close (rfd[0]);
      close (rfd[1]);
    }
    goto cleanup0;
  }

  if (r == 0) {			/* Child (qemu). */
    char buf[256];
    int virtio_scsi = qemu_supports_virtio_scsi (g);

    /* Set up the full command line.  Do this in the subprocess so we
     * don't need to worry about cleaning up.
     */

    /* Set g->cmdline[0] to the name of the qemu process.  However
     * it is possible that no g->cmdline has been allocated yet so
     * we must do that first.
     */
    alloc_cmdline (g);
    g->cmdline[0] = g->qemu;

    /* CVE-2011-4127 mitigation: Disable SCSI ioctls on virtio-blk
     * devices.  The -global option must exist, but you can pass any
     * strings to it so we don't need to check for the specific virtio
     * feature.
     */
    if (qemu_supports (g, "-global")) {
      add_cmdline (g, "-global");
      add_cmdline (g, "virtio-blk-pci.scsi=off");
    }

    if (qemu_supports (g, "-nodefconfig"))
      add_cmdline (g, "-nodefconfig");

    /* Newer versions of qemu (from around 2009/12) changed the
     * behaviour of monitors so that an implicit '-monitor stdio' is
     * assumed if we are in -nographic mode and there is no other
     * -monitor option.  Only a single stdio device is allowed, so
     * this broke the '-serial stdio' option.  There is a new flag
     * called -nodefaults which gets rid of all this default crud, so
     * let's use that to avoid this and any future surprises.
     */
    if (qemu_supports (g, "-nodefaults"))
      add_cmdline (g, "-nodefaults");

    add_cmdline (g, "-nographic");

    /* Add drives */
    struct drive *drv = g->drives;
    size_t drv_index = 0;

    if (virtio_scsi) {
      /* Create the virtio-scsi bus. */
      add_cmdline (g, "-device");
      add_cmdline (g, "virtio-scsi-pci,id=scsi");
    }

    while (drv != NULL) {
      /* Construct the final -drive parameter. */
      char *buf = qemu_drive_param (g, drv, drv_index);

      add_cmdline (g, "-drive");
      add_cmdline (g, buf);
      free (buf);

      if (virtio_scsi && drv->iface == NULL) {
        char buf2[64];
        snprintf (buf2, sizeof buf2, "scsi-hd,drive=hd%zu", drv_index);
        add_cmdline (g, "-device");
        add_cmdline (g, buf2);
      }

      drv = drv->next;
      drv_index++;
    }

    char appliance_root[64] = "";

    /* Add the ext2 appliance drive (after all the drives). */
    if (appliance) {
      const char *cachemode = "";
      if (qemu_supports (g, "cache=")) {
        if (qemu_supports (g, "unsafe"))
          cachemode = ",cache=unsafe";
        else if (qemu_supports (g, "writeback"))
          cachemode = ",cache=writeback";
      }

      char buf2[PATH_MAX + 64];
      add_cmdline (g, "-drive");
      snprintf (buf2, sizeof buf2, "file=%s,snapshot=on,if=%s%s",
                appliance, virtio_scsi ? "none" : "virtio", cachemode);
      add_cmdline (g, buf2);

      if (virtio_scsi) {
        add_cmdline (g, "-device");
        add_cmdline (g, "scsi-hd,drive=appliance");
      }

      snprintf (appliance_root, sizeof appliance_root, "root=/dev/%cd",
                virtio_scsi ? 's' : 'v');
      drive_name (drv_index, &appliance_root[12]);
    }

    if (STRNEQ (QEMU_OPTIONS, "")) {
      /* Add the extra options for the qemu command line specified
       * at configure time.
       */
      add_cmdline_shell_unquoted (g, QEMU_OPTIONS);
    }

    /* The qemu -machine option (added 2010-12) is a bit more sane
     * since it falls back through various different acceleration
     * modes, so try that first (thanks Markus Armbruster).
     */
    if (qemu_supports (g, "-machine")) {
      add_cmdline (g, "-machine");
      add_cmdline (g, "accel=kvm:tcg");
    } else {
      /* qemu sometimes needs this option to enable hardware
       * virtualization, but some versions of 'qemu-kvm' will use KVM
       * regardless (even where this option appears in the help text).
       * It is rumoured that there are versions of qemu where supplying
       * this option when hardware virtualization is not available will
       * cause qemu to fail, so we we have to check at least that
       * /dev/kvm is openable.  That's not reliable, since /dev/kvm
       * might be openable by qemu but not by us (think: SELinux) in
       * which case the user would not get hardware virtualization,
       * although at least shouldn't fail.  A giant clusterfuck with the
       * qemu command line, again.
       */
      if (qemu_supports (g, "-enable-kvm") &&
          is_openable (g, "/dev/kvm", O_RDWR|O_CLOEXEC))
        add_cmdline (g, "-enable-kvm");
    }

    if (g->smp > 1) {
      snprintf (buf, sizeof buf, "%d", g->smp);
      add_cmdline (g, "-smp");
      add_cmdline (g, buf);
    }

    snprintf (buf, sizeof buf, "%d", g->memsize);
    add_cmdline (g, "-m");
    add_cmdline (g, buf);

    /* Force exit instead of reboot on panic */
    add_cmdline (g, "-no-reboot");

    /* These options recommended by KVM developers to improve reliability. */
#ifndef __arm__
    /* qemu-system-arm advertises the -no-hpet option but if you try
     * to use it, it usefully says:
     *   "Option no-hpet not supported for this target".
     * Cheers qemu developers.  How many years have we been asking for
     * capabilities?  Could be 3 or 4 years, I forget.
     */
    if (qemu_supports (g, "-no-hpet"))
      add_cmdline (g, "-no-hpet");
#endif

    if (qemu_supports (g, "-rtc-td-hack"))
      add_cmdline (g, "-rtc-td-hack");

    /* Serial console. */
    add_cmdline (g, "-serial");
    add_cmdline (g, "stdio");

    /* Null vmchannel. */
    add_cmdline (g, "-net");
    add_cmdline (g, "user,vlan=0,net=" NETWORK);
    add_cmdline (g, "-net");
    add_cmdline (g, "nic,model=virtio,vlan=0");

    snprintf (buf, sizeof buf,
              "guestfs_vmchannel=tcp:" ROUTER ":%d",
              null_vmchannel_port);
    char *vmchannel = strdup (buf);

#ifdef VALGRIND_DAEMON
    /* Set up virtio-serial channel for valgrind messages. */
    add_cmdline (g, "-chardev");
    snprintf (buf, sizeof buf, "file,path=%s/valgrind.log.%d,id=valgrind",
              VALGRIND_LOG_PATH, getpid ());
    add_cmdline (g, buf);
    add_cmdline (g, "-device");
    add_cmdline (g, "virtserialport,chardev=valgrind,name=org.libguestfs.valgrind");
#endif

#if defined(__arm__)
#define SERIAL_CONSOLE "ttyAMA0"
#else
#define SERIAL_CONSOLE "ttyS0"
#endif

#define LINUX_CMDLINE							\
    "panic=1 "         /* force kernel to panic if daemon exits */	\
    "console=" SERIAL_CONSOLE " " /* serial console */		        \
    "udevtimeout=600 " /* good for very slow systems (RHBZ#480319) */	\
    "no_timer_check "  /* fix for RHBZ#502058 */                        \
    "acpi=off "        /* we don't need ACPI, turn it off */		\
    "printk.time=1 "   /* display timestamp before kernel messages */   \
    "cgroup_disable=memory " /* saves us about 5 MB of RAM */

    /* Linux kernel command line. */
    snprintf (buf, sizeof buf,
              LINUX_CMDLINE
              "%s "             /* (root) */
              "%s "             /* (selinux) */
              "%s "             /* (vmchannel) */
              "%s "             /* (verbose) */
              "TERM=%s "        /* (TERM environment variable) */
              "%s",             /* (append) */
              appliance_root,
              g->selinux ? "selinux=1 enforcing=0" : "selinux=0",
              vmchannel,
              g->verbose ? "guestfs_verbose=1" : "",
              getenv ("TERM") ? : "linux",
              g->append ? g->append : "");

    add_cmdline (g, "-kernel");
    add_cmdline (g, kernel);
    add_cmdline (g, "-initrd");
    add_cmdline (g, initrd);
    add_cmdline (g, "-append");
    add_cmdline (g, buf);

    /* Finish off the command line. */
    incr_cmdline_size (g);
    g->cmdline[g->cmdline_size-1] = NULL;

    if (!g->direct) {
      /* Set up stdin, stdout, stderr. */
      close (0);
      close (1);
      close (wfd[1]);
      close (rfd[0]);

      /* Stdin. */
      if (dup (wfd[0]) == -1) {
      dup_failed:
        perror ("dup failed");
        _exit (EXIT_FAILURE);
      }
      /* Stdout. */
      if (dup (rfd[1]) == -1)
        goto dup_failed;

      /* Particularly since qemu 0.15, qemu spews all sorts of debug
       * information on stderr.  It is useful to both capture this and
       * not confuse casual users, so send stderr to the pipe as well.
       */
      close (2);
      if (dup (rfd[1]) == -1)
        goto dup_failed;

      close (wfd[0]);
      close (rfd[1]);
    }

    /* Dump the command line (after setting up stderr above). */
    if (g->verbose)
      print_qemu_command_line (g, g->cmdline);

    /* Put qemu in a new process group. */
    if (g->pgroup)
      setpgid (0, 0);

    setenv ("LC_ALL", "C", 1);

    TRACE0 (launch_run_qemu);

    execv (g->qemu, g->cmdline); /* Run qemu. */
    perror (g->qemu);
    _exit (EXIT_FAILURE);
  }

  /* Parent (library). */
  g->pid = r;

  free (kernel);
  kernel = NULL;
  free (initrd);
  initrd = NULL;
  free (appliance);
  appliance = NULL;

  /* Fork the recovery process off which will kill qemu if the parent
   * process fails to do so (eg. if the parent segfaults).
   */
  g->recoverypid = -1;
  if (g->recovery_proc) {
    r = fork ();
    if (r == 0) {
      int i, fd, max_fd;
      struct sigaction sa;
      pid_t qemu_pid = g->pid;
      pid_t parent_pid = getppid ();

      /* Remove all signal handlers.  See the justification here:
       * https://www.redhat.com/archives/libvir-list/2008-August/msg00303.html
       * We don't mask signal handlers yet, so this isn't completely
       * race-free, but better than not doing it at all.
       */
      memset (&sa, 0, sizeof sa);
      sa.sa_handler = SIG_DFL;
      sa.sa_flags = 0;
      sigemptyset (&sa.sa_mask);
      for (i = 1; i < NSIG; ++i)
        sigaction (i, &sa, NULL);

      /* Close all other file descriptors.  This ensures that we don't
       * hold open (eg) pipes from the parent process.
       */
      max_fd = sysconf (_SC_OPEN_MAX);
      if (max_fd == -1)
        max_fd = 1024;
      if (max_fd > 65536)
        max_fd = 65536; /* bound the amount of work we do here */
      for (fd = 0; fd < max_fd; ++fd)
        close (fd);

      /* It would be nice to be able to put this in the same process
       * group as qemu (ie. setpgid (0, qemu_pid)).  However this is
       * not possible because we don't have any guarantee here that
       * the qemu process has started yet.
       */
      if (g->pgroup)
        setpgid (0, 0);

      /* Writing to argv is hideously complicated and error prone.  See:
       * http://git.postgresql.org/gitweb/?p=postgresql.git;a=blob;f=src/backend/utils/misc/ps_status.c;hb=HEAD
       */

      /* Loop around waiting for one or both of the other processes to
       * disappear.  It's fair to say this is very hairy.  The PIDs that
       * we are looking at might be reused by another process.  We are
       * effectively polling.  Is the cure worse than the disease?
       */
      for (;;) {
        if (kill (qemu_pid, 0) == -1) /* qemu's gone away, we aren't needed */
          _exit (EXIT_SUCCESS);
        if (kill (parent_pid, 0) == -1) {
          /* Parent's gone away, qemu still around, so kill qemu. */
          kill (qemu_pid, 9);
          _exit (EXIT_SUCCESS);
        }
        sleep (2);
      }
    }

    /* Don't worry, if the fork failed, this will be -1.  The recovery
     * process isn't essential.
     */
    g->recoverypid = r;
  }

  if (!g->direct) {
    /* Close the other ends of the pipe. */
    close (wfd[0]);
    close (rfd[1]);

    if (fcntl (wfd[1], F_SETFL, O_NONBLOCK) == -1 ||
        fcntl (rfd[0], F_SETFL, O_NONBLOCK) == -1) {
      perrorf (g, "fcntl");
      goto cleanup1;
    }

    g->fd[0] = wfd[1];		/* stdin of child */
    g->fd[1] = rfd[0];		/* stdout of child */
    wfd[1] = rfd[0] = -1;
  } else {
    g->fd[0] = open ("/dev/null", O_RDWR|O_CLOEXEC);
    if (g->fd[0] == -1) {
      perrorf (g, "open /dev/null");
      goto cleanup1;
    }
    g->fd[1] = dup (g->fd[0]);
    if (g->fd[1] == -1) {
      perrorf (g, "dup");
      close (g->fd[0]);
      g->fd[0] = -1;
      goto cleanup1;
    }
  }

  g->state = LAUNCHING;

  /* Null vmchannel implementation: We listen on g->sock for a
   * connection.  The connection could come from any local process
   * so we must check it comes from the appliance (or at least
   * from our UID) for security reasons.
   */
  r = -1;
  while (r == -1) {
    uid_t uid;

    r = guestfs___accept_from_daemon (g);
    if (r == -1)
      goto cleanup1;

    if (check_peer_euid (g, r, &uid) == -1)
      goto cleanup1;
    if (uid != geteuid ()) {
      fprintf (stderr,
               "libguestfs: warning: unexpected connection from UID %d to port %d\n",
               uid, null_vmchannel_port);
      close (r);
      r = -1;
      continue;
    }
  }

  /* Close the listening socket. */
  if (close (g->sock) != 0) {
    perrorf (g, "close: listening socket");
    close (r);
    g->sock = -1;
    goto cleanup1;
  }
  g->sock = r; /* This is the accepted data socket. */

  if (fcntl (g->sock, F_SETFL, O_NONBLOCK) == -1) {
    perrorf (g, "fcntl");
    goto cleanup1;
  }

  uint32_t size;
  void *buf = NULL;
  r = guestfs___recv_from_daemon (g, &size, &buf);
  free (buf);

  if (r == -1) {
    error (g, _("guestfs_launch failed, see earlier error messages"));
    goto cleanup1;
  }

  if (size != GUESTFS_LAUNCH_FLAG) {
    error (g, _("guestfs_launch failed, see earlier error messages"));
    goto cleanup1;
  }

  if (g->verbose)
    guestfs___print_timestamped_message (g, "appliance is up");

  /* This is possible in some really strange situations, such as
   * guestfsd starts up OK but then qemu immediately exits.  Check for
   * it because the caller is probably expecting to be able to send
   * commands after this function returns.
   */
  if (g->state != READY) {
    error (g, _("qemu launched and contacted daemon, but state != READY"));
    goto cleanup1;
  }

  TRACE0 (launch_end);

  guestfs___launch_send_progress (g, 12);

  return 0;

 cleanup1:
  if (!g->direct) {
    if (wfd[1] >= 0) close (wfd[1]);
    if (rfd[1] >= 0) close (rfd[0]);
  }
  if (g->pid > 0) kill (g->pid, 9);
  if (g->recoverypid > 0) kill (g->recoverypid, 9);
  if (g->pid > 0) waitpid (g->pid, NULL, 0);
  if (g->recoverypid > 0) waitpid (g->recoverypid, NULL, 0);
  if (g->fd[0] >= 0) close (g->fd[0]);
  if (g->fd[1] >= 0) close (g->fd[1]);
  g->fd[0] = -1;
  g->fd[1] = -1;
  g->pid = 0;
  g->recoverypid = 0;
  memset (&g->launch_t, 0, sizeof g->launch_t);

 cleanup0:
  if (g->sock >= 0) {
    close (g->sock);
    g->sock = -1;
  }
  g->state = CONFIG;
  free (kernel);
  free (initrd);
  free (appliance);
  return -1;
}

/* Alternate attach method: instead of launching the appliance,
 * connect to an existing unix socket.
 */
static int
connect_unix_socket (guestfs_h *g, const char *sockpath)
{
  int r;
  struct sockaddr_un addr;

  /* Start the clock ... */
  gettimeofday (&g->launch_t, NULL);

  /* Set these to nothing so we don't try to kill random processes or
   * read from random file descriptors.
   */
  g->pid = 0;
  g->recoverypid = 0;
  g->fd[0] = -1;
  g->fd[1] = -1;

  if (g->verbose)
    guestfs___print_timestamped_message (g, "connecting to %s", sockpath);

  g->sock = socket (AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
  if (g->sock == -1) {
    perrorf (g, "socket");
    return -1;
  }

  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, sockpath, UNIX_PATH_MAX);
  addr.sun_path[UNIX_PATH_MAX-1] = '\0';

  g->state = LAUNCHING;

  if (connect (g->sock, &addr, sizeof addr) == -1) {
    perrorf (g, "bind");
    goto cleanup;
  }

  if (fcntl (g->sock, F_SETFL, O_NONBLOCK) == -1) {
    perrorf (g, "fcntl");
    goto cleanup;
  }

  uint32_t size;
  void *buf = NULL;
  r = guestfs___recv_from_daemon (g, &size, &buf);
  free (buf);

  if (r == -1) return -1;

  if (size != GUESTFS_LAUNCH_FLAG) {
    error (g, _("guestfs_launch failed, unexpected initial message from guestfsd"));
    goto cleanup;
  }

  if (g->verbose)
    guestfs___print_timestamped_message (g, "connected");

  if (g->state != READY) {
    error (g, _("contacted guestfsd, but state != READY"));
    goto cleanup;
  }

  return 0;

 cleanup:
  close (g->sock);
  return -1;
}

/* launch (of the appliance) generates approximate progress
 * messages.  Currently these are defined as follows:
 *
 *    0 / 12: launch clock starts
 *    3 / 12: appliance created
 *    6 / 12: detected that guest kernel started
 *    9 / 12: detected that /init script is running
 *   12 / 12: launch completed successfully
 *
 * Notes:
 * (1) This is not a documented ABI and the behaviour may be changed
 * or removed in future.
 * (2) Messages are only sent if more than 5 seconds has elapsed
 * since the launch clock started.
 * (3) There is a gross hack in proto.c to make this work.
 */
void
guestfs___launch_send_progress (guestfs_h *g, int perdozen)
{
  struct timeval tv;

  gettimeofday (&tv, NULL);
  if (timeval_diff (&g->launch_t, &tv) >= 5000) {
    guestfs_progress progress_message =
      { .proc = 0, .serial = 0, .position = perdozen, .total = 12 };

    guestfs___progress_message_callback (g, &progress_message);
  }
}

/* Return the location of the tmpdir (eg. "/tmp") and allow users
 * to override it at runtime using $TMPDIR.
 * http://www.pathname.com/fhs/pub/fhs-2.3.html#TMPTEMPORARYFILES
 */
const char *
guestfs_tmpdir (void)
{
  const char *tmpdir;

#ifdef P_tmpdir
  tmpdir = P_tmpdir;
#else
  tmpdir = "/tmp";
#endif

  const char *t = getenv ("TMPDIR");
  if (t) tmpdir = t;

  return tmpdir;
}

/* Return the location of the persistent tmpdir (eg. "/var/tmp") and
 * allow users to override it at runtime using $TMPDIR.
 * http://www.pathname.com/fhs/pub/fhs-2.3.html#VARTMPTEMPORARYFILESPRESERVEDBETWEE
 */
const char *
guestfs___persistent_tmpdir (void)
{
  const char *tmpdir;

  tmpdir = "/var/tmp";

  const char *t = getenv ("TMPDIR");
  if (t) tmpdir = t;

  return tmpdir;
}

/* Recursively remove a temporary directory.  If removal fails, just
 * return (it's a temporary directory so it'll eventually be cleaned
 * up by a temp cleaner).  This is done using "rm -rf" because that's
 * simpler and safer, but we have to exec to ensure that paths don't
 * need to be quoted.
 */
void
guestfs___remove_tmpdir (const char *dir)
{
  pid_t pid = fork ();

  if (pid == -1) {
    perror ("remove tmpdir: fork");
    return;
  }
  if (pid == 0) {
    execlp ("rm", "rm", "-rf", dir, NULL);
    perror ("remove tmpdir: exec: rm");
    _exit (EXIT_FAILURE);
  }

  /* Parent. */
  if (waitpid (pid, NULL, 0) == -1) {
    perror ("remove tmpdir: waitpid");
    return;
  }
}

/* Compute Y - X and return the result in milliseconds.
 * Approximately the same as this code:
 * http://www.mpp.mpg.de/~huber/util/timevaldiff.c
 */
static int64_t
timeval_diff (const struct timeval *x, const struct timeval *y)
{
  int64_t msec;

  msec = (y->tv_sec - x->tv_sec) * 1000;
  msec += (y->tv_usec - x->tv_usec) / 1000;
  return msec;
}

/* Note that since this calls 'debug' it should only be called
 * from the parent process.
 */
void
guestfs___print_timestamped_message (guestfs_h *g, const char *fs, ...)
{
  va_list args;
  char *msg;
  int err;
  struct timeval tv;

  va_start (args, fs);
  err = vasprintf (&msg, fs, args);
  va_end (args);

  if (err < 0) return;

  gettimeofday (&tv, NULL);

  debug (g, "[%05" PRIi64 "ms] %s", timeval_diff (&g->launch_t, &tv), msg);

  free (msg);
}

/* This is called from the forked subprocess just before qemu runs, so
 * it can just print the message straight to stderr, where it will be
 * picked up and funnelled through the usual appliance event API.
 */
static void
print_qemu_command_line (guestfs_h *g, char **argv)
{
  int i = 0;
  int needs_quote;

  struct timeval tv;
  gettimeofday (&tv, NULL);
  fprintf (stderr, "[%05" PRIi64 "ms] ", timeval_diff (&g->launch_t, &tv));

  while (argv[i]) {
    if (argv[i][0] == '-') /* -option starts a new line */
      fprintf (stderr, " \\\n   ");

    if (i > 0) fputc (' ', stderr);

    /* Does it need shell quoting?  This only deals with simple cases. */
    needs_quote = strcspn (argv[i], " ") != strlen (argv[i]);

    if (needs_quote) fputc ('\'', stderr);
    fprintf (stderr, "%s", argv[i]);
    if (needs_quote) fputc ('\'', stderr);
    i++;
  }
}

static int test_qemu_cmd (guestfs_h *g, const char *cmd, char **ret);
static int read_all (guestfs_h *g, FILE *fp, char **ret);

/* Test qemu binary (or wrapper) runs, and do 'qemu -help' and
 * 'qemu -version' so we know what options this qemu supports and
 * the version.
 */
static int
test_qemu (guestfs_h *g)
{
  char cmd[1024];

  free (g->qemu_help);
  g->qemu_help = NULL;
  free (g->qemu_version);
  g->qemu_version = NULL;
  free (g->qemu_devices);
  g->qemu_devices = NULL;

  snprintf (cmd, sizeof cmd, "LC_ALL=C '%s' -nographic -help", g->qemu);

  /* If this command doesn't work then it probably indicates that the
   * qemu binary is missing.
   */
  if (test_qemu_cmd (g, cmd, &g->qemu_help) == -1) {
  qemu_error:
    error (g, _("command failed: %s\nerrno: %s\n\nIf qemu is located on a non-standard path, try setting the LIBGUESTFS_QEMU\nenvironment variable.  There may also be errors printed above."),
           cmd, strerror (errno));
    return -1;
  }

  g->qemu_version = safe_strdup (g, "");
  g->qemu_devices = safe_strdup (g, "");

  return 0;
}

static int
test_qemu_cmd (guestfs_h *g, const char *cmd, char **ret)
{
  FILE *fp;

  fp = popen (cmd, "r");
  if (fp == NULL)
    return -1;

  if (read_all (g, fp, ret) == -1) {
    pclose (fp);
    return -1;
  }

  if (pclose (fp) != 0)
    return -1;

  return 0;
}

static int
read_all (guestfs_h *g, FILE *fp, char **ret)
{
  int r, n = 0;
  char *p;

 again:
  if (feof (fp)) {
    *ret = safe_realloc (g, *ret, n + 1);
    (*ret)[n] = '\0';
    return n;
  }

  *ret = safe_realloc (g, *ret, n + BUFSIZ);
  p = &(*ret)[n];
  r = fread (p, 1, BUFSIZ, fp);
  if (ferror (fp))
    return -1;
  n += r;
  goto again;
}

/* Test if option is supported by qemu command line (just by grepping
 * the help text).
 *
 * The first time this is used, it has to run the external qemu
 * binary.  If that fails, it returns -1.
 *
 * To just do the first-time run of the qemu binary, call this with
 * option == NULL, in which case it will return -1 if there was an
 * error doing that.
 */
static int
qemu_supports (guestfs_h *g, const char *option)
{
  if (!g->qemu_help) {
    if (test_qemu (g) == -1)
      return -1;
  }

  if (option == NULL)
    return 1;

  return strstr (g->qemu_help, option) != NULL;
}

#if 0
/* As above but using a regex instead of a fixed string. */
static int
qemu_supports_re (guestfs_h *g, const pcre *option_regex)
{
  if (!g->qemu_help) {
    if (test_qemu (g) == -1)
      return -1;
  }

  return match (g, g->qemu_help, option_regex);
}
#endif

/* Test if device is supported by qemu (currently just greps the -device ?
 * output).
 */
static int
qemu_supports_device (guestfs_h *g, const char *device_name)
{
  if (!g->qemu_devices) {
    if (test_qemu (g) == -1)
      return -1;
  }

  return strstr (g->qemu_devices, device_name) != NULL;
}

/* Check if a file can be opened. */
static int
is_openable (guestfs_h *g, const char *path, int flags)
{
  int fd = open (path, flags);
  if (fd == -1) {
    debug (g, "is_openable: %s: %m", path);
    return 0;
  }
  close (fd);
  return 1;
}

/* Returns 1 = use virtio-scsi, or 0 = use virtio-blk. */
static int
qemu_supports_virtio_scsi (guestfs_h *g)
{
  int r;

  /* g->virtio_scsi has these values:
   *   0 = untested (after handle creation)
   *   1 = supported
   *   2 = not supported (use virtio-blk)
   *   3 = test failed (use virtio-blk)
   */
  if (g->virtio_scsi == 0) {
    r = qemu_supports_device (g, "virtio-scsi-pci");
    if (r > 0)
      g->virtio_scsi = 1;
    else if (r == 0)
      g->virtio_scsi = 2;
    else
      g->virtio_scsi = 3;
  }

  return g->virtio_scsi == 1;
}

static char *
qemu_drive_param (guestfs_h *g, const struct drive *drv, size_t index)
{
  size_t i;
  size_t len = 128;
  const char *p;
  char *r;
  const char *iface;

  len += strlen (drv->path) * 2; /* every "," could become ",," */
  if (drv->iface)
    len += strlen (drv->iface);
  if (drv->format)
    len += strlen (drv->format);

  r = safe_malloc (g, len);

  strcpy (r, "file=");
  i = 5;

  /* Copy the path in, escaping any "," as ",,". */
  for (p = drv->path; *p; p++) {
    if (*p == ',') {
      r[i++] = ',';
      r[i++] = ',';
    } else
      r[i++] = *p;
  }

  if (drv->iface)
    iface = drv->iface;
  else if (qemu_supports_virtio_scsi (g))
    iface = "none"; /* sic */
  else
    iface = "virtio";

  snprintf (&r[i], len-i, "%s%s%s%s,if=%s",
            drv->readonly ? ",snapshot=on" : "",
            drv->use_cache_off ? ",cache=off" : "",
            drv->format ? ",format=" : "",
            drv->format ? drv->format : "",
            iface);

  return r;                     /* caller frees */
}

/* https://rwmj.wordpress.com/2011/01/09/how-are-linux-drives-named-beyond-drive-26-devsdz/ */
static char *
drive_name (size_t index, char *ret)
{
  if (index >= 26)
    ret = drive_name (index/26 - 1, ret);
  index %= 26;
  *ret++ = 'a' + index;
  *ret = '\0';
  return ret;
}

/* Check the peer effective UID for a TCP socket.  Ideally we'd like
 * SO_PEERCRED for a loopback TCP socket.  This isn't possible on
 * Linux (but it is on Solaris!) so we read /proc/net/tcp instead.
 */
static int
check_peer_euid (guestfs_h *g, int sock, uid_t *rtn)
{
  struct sockaddr_in peer;
  socklen_t addrlen = sizeof peer;

  if (getpeername (sock, (struct sockaddr *) &peer, &addrlen) == -1) {
    perrorf (g, "getpeername");
    return -1;
  }

  if (peer.sin_family != AF_INET ||
      ntohl (peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
    error (g, "check_peer_euid: unexpected connection from non-IPv4, non-loopback peer (family = %d, addr = %s)",
           peer.sin_family, inet_ntoa (peer.sin_addr));
    return -1;
  }

  struct sockaddr_in our;
  addrlen = sizeof our;
  if (getsockname (sock, (struct sockaddr *) &our, &addrlen) == -1) {
    perrorf (g, "getsockname");
    return -1;
  }

  FILE *fp = fopen ("/proc/net/tcp", "r");
  if (fp == NULL) {
    perrorf (g, "/proc/net/tcp");
    return -1;
  }

  char line[256];
  if (fgets (line, sizeof line, fp) == NULL) { /* Drop first line. */
    error (g, "unexpected end of file in /proc/net/tcp");
    fclose (fp);
    return -1;
  }

  while (fgets (line, sizeof line, fp) != NULL) {
    unsigned line_our_addr, line_our_port, line_peer_addr, line_peer_port;
    int dummy0, dummy1, dummy2, dummy3, dummy4, dummy5, dummy6;
    int line_uid;

    if (sscanf (line, "%d:%08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %d",
                &dummy0,
                &line_our_addr, &line_our_port,
                &line_peer_addr, &line_peer_port,
                &dummy1, &dummy2, &dummy3, &dummy4, &dummy5, &dummy6,
                &line_uid) == 12) {
      /* Note about /proc/net/tcp: local_address and rem_address are
       * always in network byte order.  However the port part is
       * always in host byte order.
       *
       * The sockname and peername that we got above are in network
       * byte order.  So we have to byte swap the port but not the
       * address part.
       */
      if (line_our_addr == our.sin_addr.s_addr &&
          line_our_port == ntohs (our.sin_port) &&
          line_peer_addr == peer.sin_addr.s_addr &&
          line_peer_port == ntohs (peer.sin_port)) {
        *rtn = line_uid;
        fclose (fp);
        return 0;
      }
    }
  }

  error (g, "check_peer_euid: no matching TCP connection found in /proc/net/tcp");
  fclose (fp);
  return -1;
}

/* You had to call this function after launch in versions <= 1.0.70,
 * but it is now a no-op.
 */
int
guestfs__wait_ready (guestfs_h *g)
{
  if (g->state != READY)  {
    error (g, _("qemu has not been launched yet"));
    return -1;
  }

  return 0;
}

int
guestfs__kill_subprocess (guestfs_h *g)
{
  if (g->state == CONFIG) {
    error (g, _("no subprocess to kill"));
    return -1;
  }

  debug (g, "sending SIGTERM to process %d", g->pid);

  if (g->pid > 0) kill (g->pid, SIGTERM);
  if (g->recoverypid > 0) kill (g->recoverypid, 9);

  return 0;
}

/* Maximum number of disks. */
int
guestfs__max_disks (guestfs_h *g)
{
  if (qemu_supports_virtio_scsi (g))
    return 255;
  else
    return 27;                  /* conservative estimate */
}

/* Access current state. */
int
guestfs__is_config (guestfs_h *g)
{
  return g->state == CONFIG;
}

int
guestfs__is_launching (guestfs_h *g)
{
  return g->state == LAUNCHING;
}

int
guestfs__is_ready (guestfs_h *g)
{
  return g->state == READY;
}

int
guestfs__is_busy (guestfs_h *g)
{
  /* There used to be a BUSY state but it was removed in 1.17.36. */
  return 0;
}

int
guestfs__get_state (guestfs_h *g)
{
  return g->state;
}
