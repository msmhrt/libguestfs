/* libguestfs - the guestfsd daemon
 * Copyright (C) 2012 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "guestfs_protocol.h"
#include "daemon.h"
#include "actions.h"
#include "optgroups.h"

#define MAX_ARGS 64

int
optgroup_fstrim_available (void)
{
  return prog_exists ("fstrim");
}

/* Takes optional arguments, consult optargs_bitmask. */
int
do_fstrim (const char *path,
           int64_t offset, int64_t length, int64_t minimumfreeextent)
{
  const char *argv[MAX_ARGS];
  size_t i = 0;
  char offset_s[64], length_s[64], mfe_s[64];
  char *err;
  int r;

  ADD_ARG (argv, i, "fstrim");

  if ((optargs_bitmask & GUESTFS_FSTRIM_OFFSET_BITMASK)) {
    if (offset < 0) {
      reply_with_error ("offset < 0");
      return -1;
    }

    snprintf (offset_s, sizeof offset_s, "%" PRIi64, offset);
    ADD_ARG (argv, i, "-o");
    ADD_ARG (argv, i, offset_s);
  }

  if ((optargs_bitmask & GUESTFS_FSTRIM_LENGTH_BITMASK)) {
    if (length <= 0) {
      reply_with_error ("length <= 0");
      return -1;
    }

    snprintf (length_s, sizeof length_s, "%" PRIi64, length);
    ADD_ARG (argv, i, "-l");
    ADD_ARG (argv, i, length_s);
  }

  if ((optargs_bitmask & GUESTFS_FSTRIM_MINIMUMFREEEXTENT_BITMASK)) {
    if (minimumfreeextent <= 0) {
      reply_with_error ("minimumfreeextent <= 0");
      return -1;
    }

    snprintf (mfe_s, sizeof mfe_s, "%" PRIi64, minimumfreeextent);
    ADD_ARG (argv, i, "-m");
    ADD_ARG (argv, i, mfe_s);
  }

  ADD_ARG (argv, i, path);
  ADD_ARG (argv, i, NULL);

  r = commandv (NULL, &err, argv);
  if (r == -1) {
    reply_with_error ("%s", err);
    free (err);
    return -1;
  }
  free (err);

  return 0;
}
