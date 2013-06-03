#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
#
# Copyright (C) 2013 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

check() {
    if [[ -x $systemdutildir/systemd ]] && [[ -x /usr/sbin/ostree-prepare-root ]]; then
       return 255
    fi

    return 1
}

depends() {
    return 0
}

install() {
    dracut_install ostree-prepare-root
    inst_simple "$moddir/ostree-prepare-root.service" "${systemdsystemunitdir}/ostree-prepare-root.service"
    mkdir -p "${initdir}${systemdsystemconfdir}/initrd-switch-root.target.wants"
    ln_r "${systemdsystemunitdir}/ostree-prepare-root.service" \
        "${systemdsystemconfdir}/initrd-switch-root.target.wants/ostree-prepare-root.service"
}

