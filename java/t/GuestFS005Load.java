/* libguestfs Java bindings
 * Copyright (C) 2009 Red Hat Inc.
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

import com.redhat.et.libguestfs.*;

public class GuestFS005Load {
    public static void main (String[] argv)
    {
        try {
            GuestFS g = new GuestFS ();
            g.close ();
        }
        catch (Exception exn) {
            System.err.println (exn);
            System.exit (1);
        }
    }
}
