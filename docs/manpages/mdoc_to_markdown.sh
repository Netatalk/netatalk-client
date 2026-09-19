#!/bin/sh
# Convert one mdoc manual page to the Markdown shape used by the website.

set -eu

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <mandoc> <input.1> <page-title>" >&2
    exit 1
fi

mandoc="$1"
input="$2"
title="$3"
tmpfile=$(mktemp "${TMPDIR:-/tmp}/netatalk-client-mandoc.XXXXXX")
trap 'rm -f "${tmpfile}"' 0 HUP INT TERM

"${mandoc}" -T markdown "${input}" > "${tmpfile}"

awk -v title="${title}" '
    # mandoc adds a terminal-style title and footer.  The website supplies its
    # own chrome, so retain only the document body.
    !started && / - General Commands Manual$/ {
        skip_blank = 1
        next
    }
    skip_blank && /^$/ {
        skip_blank = 0
        next
    }
    {
        started = 1
    }
    /^Netatalk Client - / {
        next
    }

    # Make the page title useful outside a manpage reader and avoid repeating
    # the command name in the short description below it.
    !rewrote_name && $0 == "# NAME" {
        print "# " title
        rewrote_name = 1
        trim_name = 1
        next
    }
    trim_name && /^$/ {
        print
        next
    }
    trim_name {
        sub(/^\*\*[^*]+\*\* - /, "")
        trim_name = 0
    }
    {
        print
    }
' "${tmpfile}"
