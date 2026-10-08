#!/bin/sh
# Installs a .bar from the phone's Downloads folder as a permanent, unsigned
# package (no development mode, nothing to renew), using QNX's own
# sud_install_package from /base/scripts/sudtools.sh.
#
# Needs a ROOT shell: open Term49 and type  __root  first.
#
#   cat /accounts/1000/shared/downloads/installbar.sh | sh -s -- Feather-1.0.0.1-armv7.bar
#   cat /accounts/1000/shared/downloads/installbar.sh | sh -s     (no file: lists the .bar files)
#
# Why the pipe: the shared partition refuses to run files from it, so plain
# "sh installbar.sh" fails with "Operation not permitted"; feeding the script
# through stdin avoids that.
#
# The .bar is COPIED to /tmp (the tool wants a full path it can execute, owned by
# root) and the copy is removed afterwards; the original in Downloads stays.

# The __root shell of Term49 can start with a PATH that does not contain the
# system tools (ls, cp, chown, rm...): put the standard ones back.
PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export PATH

DOWNLOADS=/accounts/1000/shared/downloads

if [ -z "$1" ]; then
    echo "Usage: cat $DOWNLOADS/installbar.sh | sh -s -- <file.bar>"
    echo
    echo ".bar files in $DOWNLOADS:"
    ls "$DOWNLOADS" | grep '\.bar$' || echo "  (none)"
    exit 1
fi

NAME=${1##*/}                 # accept a bare name or a path, keep just the name
SRC="$DOWNLOADS/$NAME"
DST="/tmp/$NAME"

if [ ! -f "$SRC" ]; then
    echo "Not found: $SRC"
    echo ".bar files available:"
    ls "$DOWNLOADS" | grep '\.bar$' || echo "  (none)"
    exit 1
fi

cp "$SRC" "$DST" || { echo "Copy to /tmp failed"; exit 1; }

if ! chown root "$DST"; then
    rm -f "$DST"
    echo "chown failed: this needs a root shell (in Term49: type  __root  and try again)."
    exit 1
fi

if [ ! -f /base/scripts/sudtools.sh ]; then
    rm -f "$DST"
    echo "/base/scripts/sudtools.sh is missing: this OS has no sud tools."
    exit 1
fi
. /base/scripts/sudtools.sh   # defines sud_install_package

# The installer daemon runs as user "upd" and must be able to WRITE the job file
# we create in its PPS directory (group upd, mode 664). A shell with umask 022
# (e.g. over SSH) creates it 644 and the install dies with "Permission denied",
# so force 002 like the working root shell does.
umask 002

# A job left over from an earlier failed try of the same file would keep its old
# (wrong) mode when overwritten: remove it first.
[ -n "$INSTALLER_PPS" ] && rm -f "$INSTALLER_PPS/job.$NAME"

echo "Installing $NAME ..."
sud_install_package "$DST"
RC=$?

rm -f "$DST"

if [ "$RC" -eq 0 ]; then
    echo "Done: $NAME installed."
else
    echo "Installation failed (code $RC)."
fi
exit $RC
