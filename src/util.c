/*
 * Copyright © 2014 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H 
# include "config.h"  
#endif

#define _GNU_SOURCE  /* setresuid */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>  /* getpwnam */
#include <grp.h>  /* getgrouplist */
#include <drm.h>  /* DRM_IOCTL_SET_MASTER */
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/vt.h>  /* VT_GETSTATE */

#include "util.h"


#define CONFIG_FILE      "/etc/X11/xinit/config"
#define SESSION_WRAPPER  "/etc/X11/Xsession"
#define SERVER           "/usr/bin/X"

/* Helper macros: sizeof ("abc") = strlen ("abc") + 1 */
#define EVENT_DEV_NAME    "/dev/input/event%d"
#define EVENT_DEV_LENGTH  (sizeof (EVENT_DEV_NAME))
        
#define FB_DEV_NAME       "/dev/fb%d"
#define FB_DEV_LENGTH     (sizeof (FB_DEV_NAME))

#define TTY_DEV_NAME      "/dev/tty%d"
#define TTY_DEV_LENGTH    (sizeof (TTY_DEV_NAME))  /* tty is between 0 and 64 */

#define VC_DEV_NAME       "/dev/vc/%d"
#define VC_DEV_LENGTH     (sizeof (VC_DEV_NAME))  /* vc is between 0 and 64 */

#define DISPLAY_PATH      "/tmp/.X11-unix/X%d"
#define DISPLAY_LENGTH    (sizeof (DISPLAY_PATH))  /* display is between 0 and 99 */

#define DRM_DEV_NAME      "/dev/dri/card%d"
#define DRM_DEV_LENGTH    (sizeof (DRM_DEV_NAME))  /* idx is between 0 and 16 */

/* KISS non locale / LANG parsing isspace version */
#define IS_SPACE(c)       ((c) == ' ' || (c) == '\t' || (c) == '\n')


typedef enum {
   RootOnly,
   ConsoleOnly,
   Anybody
} Allowed;

typedef enum {
    FlagDropRoot     = (1 << 0),
    FlagDropRootAuto = (1 << 1),
    FlagDebug        = (1 << 2),
    FlagAllowChmod   = (1 << 3)
} Flags;


static const char *rootonly_name = "rootonly";
static const char *console_name = "console";
static const char *anybody_name = "anybody";

static const char *yes_name = "yes";      /* True */
static const char *true_name = "true";    /* True */
static const char *no_name = "no";        /* False */
static const char *false_name = "false";  /* False */
static const char *auto_name = "auto";    /* SCHROEDINGER_CAT */

static Flags u_flags = FlagAllowChmod | FlagDropRootAuto;
static Allowed allowed = ConsoleOnly;

const char *prog_name;
char *u_session = NULL;
char *u_display = NULL;
char *u_server = NULL;


/*
 * Code
 */

static char *
s_space (const char *p)
{
    char c;

    for ( c = *p; IS_SPACE (c); c = *++p )
        ;  /* NOP */

    return (char *) p;
}

static char *
s_space_right_end (const char *s, const char *e)
{
    char c;

    while ( e-- != s ) {
        c = *e;
        if ( !IS_SPACE (c) ) {
            e++;
            break;
        }
    }
    return (char *) e;
}

static char *
s_space_right (const char *s)
{
    int len;
   
    len = strlen (s);
    return s_space_right_end (s, s + len);
}

static char *
s_no_space (const char *p)
{
    char c;

    for ( c = *p; c != '\0' && !IS_SPACE (c); c = *++p )
        ;  /* NOP */

    return (char *) p;
}

char **
add_args (char **argv, char *args)
{
    char *sep, *cur;

    for ( cur = s_space (args);
          *cur != '\0';
          cur = s_space (sep + 1) ) {
        /* +1 because we know *s is not a space */
        sep = s_no_space (cur + 1);
        if ( *sep == '\0' ) {
            /* add last remaining argument */
            *argv = cur;
            break;
        }
        *sep = '\0';
        *argv++ = cur;
    }
    return argv;
}

const char *
s_basename (const char *path)
{
    const char *p;
    
    p = strrchr (path, '/');
    if ( p == NULL )
        return path;

    return p + 1;
}

char *
s_dup (const char *s)
{
    s = strdup (s);
    if ( s == NULL ) {
        error_no_memory ();
        return NULL;
    }
    return (char *) s;
}

void *
x_malloc (int size)
{
    void *p;
    
    p = malloc (size);
    if ( p == NULL ) {
        error_no_memory ();
        return NULL;
    }
    return p;
}
 
int
set_session (const char *value)
{
    free (u_session);
    u_session = s_dup (value);
    return u_session != NULL;
}

int
set_display (const char *value)
{
    free (u_display);
    u_display = strdup (value);
    return u_display != NULL;
}

int
set_server (const char *value)
{
    free (u_server);
    u_server = strdup (value);
    return u_server != NULL;
}

static char *
s_display (int num)
{
    char *s_disp;

    s_disp = x_malloc (sizeof (char) >> 2);  /* ":nn\0" */
    if ( s_disp == NULL )
        return NULL;

    snprintf (s_disp, sizeof (char) >> 2, ":%d", num);
    return s_disp;
}

static char *
find_free_display (void)
{
    char path [DISPLAY_LENGTH];
    int idx;
    struct stat st;

    for ( idx = 0; idx < 100; idx++ ) {
        snprintf (path, DISPLAY_LENGTH, DISPLAY_PATH, idx);
        if ( stat (path, &st) != 0 )
            return s_display (idx);
    }
    return NULL;
}

void
free_util (void)
{
    free (u_session);
    free (u_display);
    free (u_server);
}

static void
err_begin (const char *fmt, va_list ap)
{
    fprintf (stderr, "%s: ", prog_name);
    vfprintf (stderr, fmt, ap);
}

static void
err_end (void)
{
    fprintf (stderr, ": %s\n", strerror (errno));
}

void
debug (const char *fmt, ...)
{
    va_list ap;

    if ( (u_flags & FlagDebug) == 0 )
        return;

    va_start(ap, fmt);
    err_begin (fmt, ap);
    err_end ();
    va_end(ap);
}

void
debugx (const char *fmt, ...)
{
    va_list ap;

    if ( (u_flags & FlagDebug) == 0 )
        return;

    va_start(ap, fmt);
    err_begin (fmt, ap);
    fputc ( '\n', stderr);
    va_end(ap);
}

static const char *
s_allowed (Allowed value)
{
    if ( value == Anybody )
        return anybody_name;
    
    if ( value == RootOnly )
        return rootonly_name;
    
    /* ConsoleOnly */
    return console_name;
}

static const char *
s_bool (int value)
{
    /* NO */
    if ( !value )
        return false_name;

    /* YES */
    return true_name;
}

#if 0
static const char *
s_bool_auto (int value)
{
    /* NO */
    if ( !value )
        return false_name;

    /* YES */
    if ( value == True )
        return true_name;

    /* AUTO */
    return auto_name;
}
#endif

static int
parse_int (const char *value)
{
    if (strcmp (value, true_name) == 0 || strcmp (value, yes_name) == 0)
        return True;

    if (strcmp (value, false_name) == 0 || strcmp (value, no_name) == 0)
        return False;

    return SCHROEDINGER_CAT;
}

int
parse_config (void)
{
    FILE *config;
    char buf [1024];
    char *temp, *key, *val_s;
    int val_i, line = 0;

    find_free_display ();

    config = fopen (CONFIG_FILE, "r");
    if ( config == NULL ) {
        debugx ("could not open config file %s, using default values:\n allowed=%s\n drop-root=%s\n allow-chmod=%s\n session-wrapper=%s\n u_display=%s\n u_server=%s",
            CONFIG_FILE,
            s_allowed (allowed),
            s_bool (u_flags & (FlagDropRoot | FlagDropRootAuto)),
            s_bool (u_flags & FlagAllowChmod),
            u_session, u_display, u_server);
        
        return True; /* We return true because we don't want to terminate the process */
    }

    while ( fgets (buf, sizeof (buf), config) ) {
        line++;

        /* Skip comments and empty lines */
        key = s_space (buf);
        if ( *key == '#' || *key == '\0' )
            continue;

        /* Split in a key + value pair */
        temp = strchr (key, '=');
        if ( temp == NULL ) {
            errorx ("missing '=' at line %d", line);
            goto quit;
        }

        val_s = s_space (temp + 1);

        /* To remove trailing whitespace from key */
        temp = s_space_right_end (key, temp);
        if ( temp == key ) {
            errorx ("missing key at line %d", line);
            goto quit;
        }
        
        *temp = '\0';

        /* To remove leading whitespace from value */
        temp = s_space_right (val_s);
        if ( temp == val_s ) {
            errorx ("missing value at line %d", line);
            goto quit;
        }
        
        *temp = '\0';

        debugx ("config: key='%s' value='%s'", key, val_s);

        /* And finally process */
        if (strcmp(key, "allowed-users") == 0) {
            if (strcmp (val_s, rootonly_name) == 0)
                allowed = RootOnly;
            else if (strcmp (val_s, console_name) == 0)
                allowed = ConsoleOnly;
            else if (strcmp (val_s, anybody_name) == 0)
                allowed = Anybody;
            else {
                errorx ("invalid value '%s' for 'allowed_users' at line %d", val_s, line);
                goto quit;
            }
        }
        else if (strcmp(key, "drop-root") == 0) {
            u_flags &= ~(FlagDropRoot | FlagDropRootAuto);

            val_i = parse_int (val_s);
            if ( val_i == SCHROEDINGER_CAT ) {
                if ( strcmp (val_s, auto_name) != 0 ) {
                    errorx ("invalid value '%s' for 'drop-root' at line %d", val_s, line);
                    goto quit;
                }
                u_flags |= FlagDropRootAuto;
            } else if ( val_i )
                u_flags |= FlagDropRoot;
        }
        else if (strcmp(key, "debug") == 0) {
            u_flags &= ~FlagDebug;

            val_i = parse_int (val_s);
            if ( val_i == SCHROEDINGER_CAT ) {
                errorx ("invalid value '%s' for 'debug' at line %d", val_s, line);
                goto quit;
            }
            if ( val_i )
                u_flags |= FlagDebug;
        }
        else if (strcmp(key, "allow-chmod") == 0) {
            u_flags &= ~FlagAllowChmod;

            val_i = parse_int (val_s);
            if ( val_i == SCHROEDINGER_CAT ) {
                errorx ("invalid value '%s' for 'allow-chmod' at line %d", val_s, line);
                goto quit;
            }
            if ( val_i )
                u_flags |= FlagAllowChmod;
        }
        else if (strcmp (key, "session-wrapper") == 0) {
            if ( !set_session (val_s) )
                goto quit;
        } else if (strcmp (key, "u_display") == 0) {
            if ( !set_display (val_s) )
                goto quit;
        } else if (strcmp (key, "u_server") == 0) {
            if ( !set_server (val_s) )
                goto quit;
        } else {
            errorx ("invalid key '%s' at line %d", key, line);
            goto quit;
        }
    }

    debugx ("parsed config file %s, using following values:\n allowed=%s\n drop-root=%s\n allow-chmod=%s\n session-wrapper=%s\n u_display=%s\n u_server=%s", CONFIG_FILE,
           s_allowed (allowed),
           s_bool (u_flags & (FlagDropRoot | FlagDropRootAuto)),
           s_bool (u_flags & FlagAllowChmod),
           u_session, u_display, u_server);

    fclose (config);
    return True;

quit:

    fclose (config);
    return False;
}

static int
check_user_rights (struct stat *st, uid_t uid, int read, int write)
{
    if ( st->st_uid != uid )
        return False;

    if ( read && !(st->st_mode & S_IRUSR) )
        return False;

    if ( write && !(st->st_mode & S_IWUSR) )
        return False;

    return True;
}

static int
group_list_find (gid_t *grouplist, int ngroups, gid_t value)
{
    int idx;

    for ( idx = 0; idx < ngroups; idx++ ) {
        if ( *grouplist++ == value )
            return idx;
    }
    return -1;
}

static int
check_group_rights (struct stat *st, gid_t *grouplist, int ngroups, int read, int write)
{
    if ( group_list_find (grouplist, ngroups, st->st_gid ) == -1 )
        return False;

    if ( read && !(st->st_mode & S_IRGRP) )
        return False;

    if ( write && !(st->st_mode & S_IWGRP) )
        return False;

    return True;
}

static int
check_other_rights (struct stat *st, int read, int write)
{
    if ( read && !(st->st_mode & S_IROTH) )
        return False;

    if ( write && !(st->st_mode & S_IWOTH) )
        return False;

    return True;
}

static void
str_vstate (char *buf, int val)
{
    int mask;
    
    for ( mask = 1 << 15; mask != 0; mask >>= 1 ) {
        *buf++ = (val & mask) ? '1' : '0';
    }
    *buf = '\0';
}

static int
dev_has_rights (uid_t uid, gid_t *grouplist, int ngroups, const char *dev, int fd, int read, int write)
{
    struct stat dev_stat;
    int result;
    char buf [17];

    /* Get device properties */
    if ( fd != -1 )
        result = fstat (fd, &dev_stat);
    else
        result = stat (dev, &dev_stat);

    if ( result == -1 ) {
        debug ("could not read stats for %s", dev);
        return DIE;
    }
    /* Other read/write permissions */
    if ( check_other_rights (&dev_stat, read, write) )
        return True;
    /* User read/write permissions */
    if ( check_user_rights (&dev_stat, uid, read, write) )
        return True;
    /* Group read/write permissions */
    if ( check_group_rights (&dev_stat, grouplist, ngroups, read, write) )
        return True;

    str_vstate (buf, dev_stat.st_mode);
    debugx ("device %s does not have necessary permissions: owner=%d group=%d mode=%s",
           dev, dev_stat.st_uid, dev_stat.st_gid, buf);
    return False;
}

static int
tty_dev_chmod (int idx)
{
    struct stat tty_stat;
    char tty_name [TTY_DEV_LENGTH]; /* max tty idx is 64 */
    char src [17], dest [17];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf (tty_name, TTY_DEV_LENGTH, TTY_DEV_NAME, idx);  /* /dev/tty$idx */
#pragma GCC diagnostic pop

    /* We'll read stats and then we'll update 'rw' permissions for group */
    if ( stat (tty_name, &tty_stat) == -1 ) {
        debug ("could not read stats for %s", tty_name);
        return DIE;
    }
    str_vstate (src, tty_stat.st_mode);
    tty_stat.st_mode |= S_IRGRP | S_IWGRP;
    str_vstate (dest, tty_stat.st_mode);

    if ( chmod (tty_name, tty_stat.st_mode) == -1 ) {
        debug ("could not change permissions for %s (%s -> %s)", tty_name, src, dest);
        return DIE;
    }
    debugx ("changed permissions for %s (%s -> %s)", tty_name, src, dest);
    return True;
}

static int
tty_zero_dev_has_rights (uid_t uid, gid_t *grouplist, int ngroups, int *vstate)
{
    char tty_name [TTY_DEV_LENGTH]; /* max tty idx is 64 */
    int fd, result;
    struct vt_stat vts;
    char buf[17]; /* max short (0xFFFF) has 16 bits + null char */
    
    snprintf (tty_name, TTY_DEV_LENGTH, TTY_DEV_NAME, 0);  /* /dev/tty$idx */

    fd = open (tty_name, O_RDONLY, 0);
    if ( fd == -1 ) { 
        debug ("could not open %s", tty_name);
        return (errno == EACCES) ? False : DIE;
    }

    result = dev_has_rights (uid, grouplist, ngroups, tty_name, fd, False, True);
    if ( result != True )  /* Attn: don't change to `if (!result)` due to DIE result */
        return result;

    if ( ioctl (fd, VT_GETSTATE, &vts) == -1 ) {
        debug ("%s: could not find the current VT", tty_name );
        goto quit;
    }

    str_vstate (buf, vts.v_state);
    debugx ("opened %s: current VT=%d active VTs (mask)=%s",
        tty_name, vts.v_active, buf);

    /* Close tty */
    close (fd);

    *vstate = vts.v_state;
    return vts.v_active;

quit:

    /* Close tty device */
    close (fd);
    return DIE;
}

static int
events_have_rights (uid_t uid, gid_t *grouplist, int ngroups)
{
    int idx, result;
    char event_name [EVENT_DEV_LENGTH];

    for ( idx = 0; idx < 32; idx++ ) {
        snprintf (event_name, EVENT_DEV_LENGTH, EVENT_DEV_NAME, idx);  /* /dev/input/event$idx */
        result = dev_has_rights (uid, grouplist, ngroups, event_name, -1, True, False);
        if ( result == DIE )
            return DIE;

        if ( result ) {
            debugx ("found input device %s with necessary permissions", event_name);
            return True;
        }
    }

    debugx ("(!) consider adding the user to 'input' group");
    return False;
}

static int
ttys_have_rights (uid_t uid, gid_t *grouplist, int ngroups, int shareVTs)
{
    int idx, vtno, vstate, vtfree = -1;
    int mask = 1 << 1;  /* The loop starts from tty1 (1 belongs to tty0) */
    char tty_name [TTY_DEV_LENGTH]; /* max tty idx is 64 */

    /* Is the current user in the tty group?? */
    vtno = tty_zero_dev_has_rights (uid, grouplist, ngroups, &vstate);
    if ( vtno == DIE )
        return DIE;

    if ( !vtno ) {
        debugx ("(!) consider adding the user to 'tty' group");
        return False;
    }
    /* 'sharevts' argument has been used */
    if ( shareVTs ) {
        snprintf (tty_name, TTY_DEV_LENGTH, TTY_DEV_NAME, vtno);  /* /dev/tty$idx */
        return dev_has_rights (uid, grouplist, ngroups, tty_name, -1, True, True);
    }

    /* Emulate VT_OPENQRY: find a free VT except the current one */
    for ( idx = 1; idx < 16; idx++, mask <<= 1 ) {
        /* Skip when a VT is used */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        if ( vstate & mask ) {
            debugx ("skipping: VT %d is not free", idx);
            continue;
        }
#pragma GCC diagnostic pop

        snprintf (tty_name, TTY_DEV_LENGTH, TTY_DEV_NAME, idx);  /* /dev/tty$idx */

        vtno = dev_has_rights (uid, grouplist, ngroups, tty_name, -1, True, True);
        if ( vtno == DIE )
            return DIE;

        if ( vtno ) {
            /* X u_server will be able to open virtual console */
            debugx ("found free VT: %d", idx);
            return True;
        }

        /* Save the first free VT index */
        if ( vtfree == -1 )
            vtfree = idx;
    }
    debugx ("could not find a free VT: check permissions");

    /* Let's try to change permissions when chmod is allowed */
    if ( u_flags & FlagAllowChmod && vtfree != -1 )
        return tty_dev_chmod (vtfree);

    return False;
}

int
is_user_allowed (gid_t uid)
{
    /* We don't need to check anything when anybody is allowed or
     * the running user is the root */
    if ( allowed == Anybody || uid == 0 )
        return True;

    /* For non root users check if they are allowed to run the X u_server */
    if ( allowed == RootOnly ) {
        /* Already checked above */
        errorx ("only root is allowed to run the X u_server");
        return False;    
    }

    /* allowed == CONSOLE_ONLY */
//    get_current_vt ();
   
//    errorx ("only console users are allowed to run the X u_server");
//    return Falsei;
    return True;
}

static int
fbs_have_rights (uid_t uid, gid_t *grouplist, int ngroups)
{
    int idx, result;
    char fb_name [FB_DEV_LENGTH];

    /* Try to find first valid framebuffer device */
    for ( idx = 0; idx < 8; idx++ ) {
        snprintf (fb_name, FB_DEV_LENGTH, FB_DEV_NAME, idx);  /* /dev/fb$idx */
        result = dev_has_rights (uid, grouplist, ngroups, fb_name, -1, True, True);
        if ( result == DIE )
            return DIE;

        if ( result ) {
            debugx ("found valid framebuffer device: %s", fb_name);
            return True;
        }
    }
    debugx ("(!) unable to find a valid framebuffer device");
    return False;
}

static void
debug_grouplist (gid_t *grouplist, int ngroups)
{
    int idx;

    if ( (u_flags & FlagDebug) == 0 )
        return;

    for ( idx = 0; idx < ngroups; idx++ ) {
        fprintf (stderr, " %d", *grouplist++);
    }
    fputc ('\n', stderr);
}

static gid_t *
get_user_groups (uid_t uid, int *ngroups)
{
    struct passwd *pwd;
    gid_t *grouplist;
    int count = 0;
    
    pwd = getpwuid (uid);
    if ( pwd == NULL ) {
        debug ("getpwuid error");
        return NULL;
    }

    getgrouplist (pwd->pw_name, pwd->pw_gid, NULL, &count );
    debugx ("user %s is a member of %d groups", pwd->pw_name, count);

    grouplist = malloc (count * sizeof (gid_t));
    if ( grouplist == NULL ) {
        error_no_memory ();
        return NULL;
    }

    getgrouplist (pwd->pw_name, pwd->pw_gid, grouplist, &count);
    debug_grouplist (grouplist, count);
    
    *ngroups = count;
    return grouplist;
}

static int
drm_dev_has_rights (int idx, uid_t uid, gid_t *grouplist, int ngroups)
{
    char drm_name [DRM_DEV_LENGTH];
    int fd, result;
    
    snprintf (drm_name, DRM_DEV_LENGTH, DRM_DEV_NAME, idx);

    fd = open (drm_name, O_RDONLY, 0);
    if ( fd == -1 ) {
        debug ("could not open %s", drm_name);
        return False;
    }

    result = dev_has_rights (uid, grouplist, ngroups, drm_name, fd, True, True);
    if ( result != True )
        goto quit;

    /* Only root can call drm_set_master and drm_drop_master in Linux kernel 4.x
     * so let's try to set drm master and don't terminate the app (DIE result) when
     * the failure is caused by missing permissions (EACCES errno) */
    if ( ioctl (fd, DRM_IOCTL_SET_MASTER, 0) == -1 ) {
        debug ("%s: drmSetMaster failed", drm_name );
        result = (errno == EACCES) ? False : DIE;
        goto quit;
    }

    if ( ioctl (fd, DRM_IOCTL_DROP_MASTER, 0) == -1 ) {
        debug ("%s: drmDropMaster failed", drm_name );
        result = (errno == EACCES) ? False : DIE;
        goto quit;
    }

    debugx ("found valid drm device %s", drm_name);

    /* Close drm */
    close (fd);
    return True;

quit:

    /* Close drm device */
    close (fd);
    return result;
}

static int
drms_have_rights (uid_t uid, gid_t *grouplist, int ngroups)
{
    int idx, result;

    for ( idx = 0; idx < 16; idx++ ) {
        result = drm_dev_has_rights (idx, uid, grouplist, ngroups);
        if ( result )
            return result;
    }

    debugx ("(!) unable to find a valid drm device, consider adding the user to 'video' group or check kernel version");
    return False;
}

static int
video_has_rights (uid_t uid, gid_t *grouplist, int ngroups)
{
    int result;
   
    /* Test framebuffer devices */
    result = fbs_have_rights (uid, grouplist, ngroups);
    if ( result != True )
        return result;

    /* Test drm cards */
    return drms_have_rights (uid, grouplist, ngroups);
}

int
drop_user_privileges (gid_t uid)
{
    /* Change user privileges */
    if ( setresuid (-1, uid, uid) != 0 ) {
         error ("could not drop user privileges");
         return False;
    }
    debugx ("user privileges dropped");
    return True;
}

static int
handle_auto_rights (uid_t uid, gid_t *grouplist, int ngroups, int shareVTs)
{
    int result = True;
    int curr;
    
    /* drm */
    curr = video_has_rights (uid, grouplist, ngroups);
    if ( curr == DIE )
        return DIE;
    if ( !curr )
        result = False;

    /* tty */
    curr = ttys_have_rights (uid, grouplist, ngroups, shareVTs);
    if ( curr == DIE )
        return DIE;
    if ( !curr )
        result = False;

    /* input */
    curr = events_have_rights (uid, grouplist, ngroups);
    if ( curr == DIE )
        return DIE;
    if ( !curr )
        result = False;
    
    return result; 
}

int
check_rights (uid_t uid, int shareVTs)
{
    gid_t *grouplist;
    int ngroups, result;

    if ( u_flags & FlagDropRootAuto ) {
        grouplist = get_user_groups (uid, &ngroups);
        if ( grouplist == NULL )
            return DIE;

        result = handle_auto_rights (uid, grouplist, ngroups, shareVTs);
        free (grouplist);
        return result;
    }

    return u_flags & FlagDropRoot;
}

int
check_execute_rights (const char *path)
{
    /* Check if the u_server is executable by the real user */
    if ( access (path, X_OK) == 0 ) {
        debugx ("user has execute permissions for %s", path);
        return True;
    }

    error ("missing execute permissions for %s", path);
    return False;
}

int
set_display_env (void)
{
    if (setenv ("DISPLAY", u_display, True) != -1)
        return True;

    error ("unable to set DISPLAY");
    return False;
}

void
error (const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    err_begin (fmt, ap);
    err_end ();
    va_end (ap);
}

void
errorx (const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    err_begin (fmt, ap);
    fputc ( '\n', stderr); 
    va_end (ap);
}

void
error_no_memory (void)
{
    errorx ("out of memory");
}
