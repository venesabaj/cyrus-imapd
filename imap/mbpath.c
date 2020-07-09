/* mbpath.c -- help the sysadmin to find the path matching the mailbox
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/param.h>

#include "util.h"
#include "global.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "mboxlist.h"
#include "user.h"

/* generated headers are not necessarily in current directory */
#include "imap/imap_err.h"

extern int optind;
extern char *optarg;

/* current namespace */
static struct namespace mbpath_namespace;

static int usage(const char *error)
{
    fprintf(stderr,"usage: mbpath [-C <alt_config>] [-l] [-m] [-q] [-s] [-u|d] [-a|A|D|M|S|U] <mailbox name>...\n");
    fprintf(stderr, "\n");
    fprintf(stderr,"\t-a\tprint all values with prefixes\n");
    fprintf(stderr,"\t-l\tlocal only (exit with error for remote/nonexistent)\n");
    fprintf(stderr,"\t-m\toutput the path to the metadata files (if different from the message files)\n");
    fprintf(stderr,"\t-q\tquietly drop any error messages\n");
    fprintf(stderr,"\t-s\tstop on error\n");
    fprintf(stderr,"\t-u\targuments are user, not mailbox\n");
    fprintf(stderr,"\t-d\targuments are domain, not mailbox\n");
    fprintf(stderr,"\t-A\tpartition archive directory\n");
    fprintf(stderr,"\t-D\tpartition data directory (*default*)\n");
    fprintf(stderr,"\t-M\tpartition metadata file directory (duplicate of -m)\n");
    fprintf(stderr,"\t-S\tsieve directory for the user\n");
    fprintf(stderr,"\t-U\tuser files directory (seen, sub, etc)\n");
    if (error) {
        fprintf(stderr,"\n");
        fprintf(stderr,"ERROR: %s", error);
    }
    exit(-1);
}

struct options_t {
    unsigned quiet         : 1;
    unsigned stop_on_error : 1;
    unsigned localonly     : 1;
    unsigned foundone      : 1;
    unsigned mode          : 2;
    unsigned paths         : 5;
};

#define DO_ARCHIVE  (1<<0)
#define DO_DATA     (1<<1)
#define DO_META     (1<<2)
#define DO_SIEVE    (1<<3)
#define DO_USER     (1<<4)
#define DO_ALL      (DO_ARCHIVE | DO_DATA | DO_META | DO_SIEVE | DO_USER)

#define MODE_USER   1
#define MODE_DOMAIN 2

static int do_paths(struct findall_data *data, void *rock)
{
    struct options_t *opts = (struct options_t *) rock;

    /* don't want partial matches */
    if (!data || !data->is_exactmatch) return 0;

    /* Ignore "reserved" entries, like they aren't there */
    if (data->mbentry->mbtype & MBTYPE_RESERVE) {
        return IMAP_MAILBOX_RESERVED;
    }
    /* Ignore "deleted" entries, like they aren't there */
    else if (data->mbentry->mbtype & MBTYPE_DELETED) {
        return IMAP_MAILBOX_NONEXISTENT;
    }
    else if (data->mbentry->mbtype & MBTYPE_REMOTE) {
        if (opts->localonly) {
            if (opts->stop_on_error) {
                if (opts->quiet) {
                    fatal("", -1);
                }
                else {
                    fatal("Non-local mailbox. Stopping\n", -1);
                }
            }
        }
        else {
            // ignore all paths and just print this
            printf("%s!%s\n", data->mbentry->server, data->mbentry->partition);
        }
    }
    else {
        if (opts->mode == MODE_DOMAIN) {
            printf("\nUserid: %s\n", mbname_userid(data->mbname));
        }
        if (opts->paths & DO_ARCHIVE) {
            const char *path = mbentry_archivepath(data->mbentry, 0);
            if (opts->paths == DO_ALL) printf("Archive: ");
            printf("%s\n", path);
        }
        if (opts->paths & DO_DATA) {
            const char *path = mbentry_datapath(data->mbentry, 0);
            if (opts->paths == DO_ALL) printf("Data: ");
            printf("%s\n", path);
        }
        if (opts->paths & DO_META) {
            const char *path = mbentry_metapath(data->mbentry, 0, 0);
            if (opts->paths == DO_ALL) printf("Meta: ");
            printf("%s\n", path);
        }
        if (opts->paths & DO_SIEVE) {
            const char *path = user_sieve_path(mbname_userid(data->mbname));
            if (opts->paths == DO_ALL) printf("Sieve: ");
            printf("%s\n", path);
        }
        if (opts->paths & DO_USER) {
            // different interface - caller must free
            char *path = mboxname_conf_getpath(data->mbname, NULL);
            if (opts->paths == DO_ALL) printf("User: ");
            printf("%s\n", path);
            free(path);
        }
    }

    opts->foundone = 1;

    return 0;
}

int main(int argc, char **argv)
{
    int r, i;
    int opt;              /* getopt() returns an int */
    char *alt_config = NULL;

    // capture options
    struct options_t opts = { 0, 0, 0, 0, 0, 0 };

    while ((opt = getopt(argc, argv, "C:almqsudADMSU")) != EOF) {
        switch(opt) {
        case 'C': /* alt config file */
            alt_config = optarg;
            break;

        case 'a':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_ALL;
            break;

        case 'l':
            opts.localonly = 1;
            break;

        case 'm':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_META;
            break;

        case 'q':
            opts.quiet = 1;
            break;

        case 's':
            opts.stop_on_error = 1;
            break;

        case 'u':
            if (opts.mode)
                usage("Multiple modes given");
            opts.mode = MODE_USER;
            break;

        case 'd':
            if (opts.mode)
                usage("Multiple modes given");
            opts.mode = MODE_DOMAIN;
            break;

        case 'A':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_ARCHIVE;
            break;

        case 'D':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_DATA;
            break;

        case 'M':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_META;
            break;

        case 'S':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_SIEVE;
            break;

        case 'U':
            if (opts.paths)
                usage("Duplicate selectors given");
            opts.paths = DO_USER;
            break;

        default:
            usage(NULL);
        }
    }

    if (!opts.paths) opts.paths = DO_DATA; // default

    cyrus_init(alt_config, "mbpath", 0, 0);


    r = mboxname_init_namespace(&mbpath_namespace, 1);
    if (r) {
        fatal(error_message(r), -1);
    }

    for (i = optind; i < argc; i++) {
        /* Translate mailboxname */
        mbname_t *mbname = NULL;
        const char *extname;

        if (opts.mode == MODE_DOMAIN) {
            char *userid = strconcat("%@", argv[i], NULL); // all inboxen
            mbname = mbname_from_userid(userid);
            free(userid);
        }
        else if (opts.mode == MODE_USER) {
            mbname = mbname_from_userid(argv[i]);
        }
        else {
            mbname = mbname_from_path(argv[i], &mbpath_namespace);
        }

        extname = mbname_extname(mbname, &mbpath_namespace, "cyrus");
        if (extname) {
            r = mboxlist_findall(&mbpath_namespace, extname,
                                 /*isadmin*/1, NULL, NULL, &do_paths, &opts);
            if (!r && !opts.foundone) {
                r = IMAP_MAILBOX_NONEXISTENT;
            }
        }
        else {
            r = IMAP_MAILBOX_NONEXISTENT;
        }
        if (r) {
            if (!opts.quiet && (r == IMAP_MAILBOX_NONEXISTENT)) {
                fprintf(stderr, "Invalid mailbox name: %s\n", argv[i]);
                if (extname) fprintf(stderr, "Invalid mailbox name: %s\n", extname);
            }
            if (opts.stop_on_error) {
                if (opts.quiet) {
                    fatal("", -1);
                }
                else {
                    fatal("Error in processing mailbox. Stopping\n", -1);
                }
            }
        }
        mbname_free(&mbname);
    }

    cyrus_done();

    return 0;
}
