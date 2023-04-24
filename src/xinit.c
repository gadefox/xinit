/*
 * Copyright 1986, 1998  The Open Group
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of The Open Group shall not be
 * used in advertising or otherwise to promote the sale, use or other dealings
 * in this Software without prior written authorization from The Open Group.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <unistd.h>

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
#include <vproc.h>
#endif
#endif

/* For PRIO_PROCESS and setpriority() */
#include <sys/time.h>
#include <sys/resource.h>

#include <stdlib.h>

#include "util.h"


#ifndef SHELL
  #define SHELL  "/usr/bin/sh"
#endif


const char * const server_names[] = {
#ifdef __APPLE__
    "Xquartz     Mac OSX Quartz displays.",
#else
# ifdef __CYGWIN__
    "XWin        X Server for the Cygwin environment on Microsoft Windows",
# else
    "Xorg        Common X server for most displays",
# endif
#endif
    "Xvfb        Virtual frame buffer",
    "Xfake       kdrive-based virtual frame buffer",
    "Xnest       X server nested in a window on another X server",
    "Xephyr      kdrive-based nested X server",
    "Xvnc        X server accessed over VNC's RFB protocol",
    "Xdmx        Distributed Multi-head X server",
    NULL
};

static const char xinitrc [] = "/xorg/xinitrc";
static char xinitrcbuf [256];
static char *clientargv [96];
static char **client = clientargv + 2;  /* make sure room for sh .xinitrc args */
static pid_t clientpid = -1;

static const char xserverrc [] = "/xorg/xserverrc";
static char xserverrcbuf [256];
static char *serverargv [96];
static char **server = serverargv + 2;  /* make sure room for sh .xserverrc args */
static pid_t serverpid = -1;

#ifdef __sun
static const char *kbd_mode = "/usr/bin/kbd_mode";
#endif

static Display *xd = NULL;            /* server connection */
static volatile int gotSignal = 0;
static int status;   

static void ExecuteXorg (char **vec, Bool elevated_rights);
static void ExecuteRelative (char **vec); 
static Bool waitforserver (void);
static Bool processTimeout (int timeout, const char *string);
static pid_t startServer (char *server[], Bool use_execve);
static pid_t startClient (char *client[], uid_t euid, uid_t uid);
static int ignorexio (Display *dpy);
static Bool shutdown (void);


/*
 * Code
 */

static void
sigCatch (int sig)
{
    /* On system with POSIX signals, just interrupt the system call */
    gotSignal = sig;
}

static void
sigIgnore (int sig)
{
    /* NOP */
}

static void
ExecuteXorg (char **argv, Bool elevated_rights)
{
    char *const empty_envp[1] = { NULL };

    if ( elevated_rights )
        execve (*argv, argv, empty_envp);
    else
        execv (*argv, argv); 
}

static void
ExecuteRelative (char **vec)  /* has room from up above */
{
    char *s = *vec;

    execvp (s, vec);

    if ( access (s, R_OK) != 0 )
        return;

    /* back it up to stuff shell in */
    s = (char *) SHELL;
    vec--;
    *vec = s;
    execvp (s, vec);
}

int
main (int argc, char *argv[])
{
#ifdef __APPLE__
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
    vproc_transaction_t vt;
#endif
#endif     
    register char **sptr;
    register char **cptr;
    int client_given = False, server_given = False;
    int start_of_client_args, start_of_server_args, result;
    struct sigaction sa, si;
    uid_t uid, euid;
    pid_t pid;
    int shareVTs = False;
    char *home, *xdg_config, *cp;
    char c;

    /*
     * Check config file parameters
     */
    prog_name = s_basename (*argv++);
    argc--;

    if ( !parse_config () )
        goto quit;

    /*
     * copy the client args.
     */
    c = argc != 0 ? **argv : '\0';
    if ( c != '/' && c != '.' )
        cptr = add_args (client, u_session);
    else {
        cptr = client;
        client_given = True;
    }
    
    start_of_client_args = cptr - client;
    while ( argc != 0 ) {
        cp = *argv;
        if ( strcmp (cp, "--") == 0 )
            break;

        if ( cptr > clientargv + countof (clientargv) - 2 ) {
            errorx ("too many client arguments");
            goto quit;
        }
        *cptr++ = cp;
        argv++;
        argc--;
    }
    
    *cptr = NULL;
    if ( argc != 0 ) {
        argv++;
        argc--;
    }

    /*
     * Copy the server args.
     */
    c = argc != 0 ? **argv : '\0';
    if ( c != '/' && c != '.' )
        sptr = add_args (server, u_server);
    else {
        sptr = server;
        server_given = True;

        *sptr++ = *argv++;
        argc--;
    }

    /* display */
    if ( argc == 0 )
        *sptr++ = u_display;
    else {
        cp = *argv;
        if ( *cp != ':' || !isdigit (cp [1]) )
            *sptr++ = u_display;
        else if ( !set_display (cp) )
            goto quit;
    }

    /* ShareVTs argument */
    start_of_server_args = sptr - server;
    while ( --argc != 0 ) {
        /* ShareVTs */
        cp = *argv++;
        if ( strcmp (cp, "-sharevts") == 0 ) {
            debugx ("found 'sharevts' argument");
            shareVTs = True;
        }
        if ( sptr > serverargv + countof (serverargv) - 2 ) {
            errorx ("too many server arguments");
            goto quit;
        }
        *sptr++ = cp;
    }
    *sptr = NULL;

    /* Is user allowed to launch X server and does (s)he really need
     * the root permissions ?? */
    uid = getuid ();
    if ( !is_user_allowed (uid) )
        goto quit;

    result = check_rights (uid, shareVTs);
    if ( result == DIE )
        goto quit;

    if ( result && !drop_user_privileges (uid) )
        goto quit;

    /*
     * if no client arguments given, check for a startup file and copy
     * that into the argument list
     */
    home = getenv ("HOME");
    xdg_config = getenv ("XDG_CONFIG_HOME");

    if ( !client_given ) {
        result = False;
        *xinitrcbuf = '\0';

        cp = getenv ("XINITRC");
        if ( cp != NULL ) {
            strcpy (xinitrcbuf, cp);
            result = True;
        } else if ( xdg_config != NULL )
            snprintf (xinitrcbuf, sizeof (xinitrcbuf), "%s%s", xdg_config, xinitrc);
        else if ( home != NULL )
            snprintf (xinitrcbuf, sizeof (xinitrcbuf), "%s/.config%s", home, xinitrc);

        if ( *xinitrcbuf != '\0' ) {
            if ( access (xinitrcbuf, R_OK) == 0 ) {
                client += start_of_client_args - 1;
                *client = xinitrcbuf;
            } else if ( result )
                error ("warning, no client init file \"%s\"", xinitrcbuf);
        }
    }
    /*
     * if no server arguments given, check for a startup file and copy
     * that into the argument list
     */
    if ( !server_given ) {
        result = False;
        *xserverrcbuf = '\0';

        cp = getenv ("XSERVERRC");
        if ( cp != NULL) {
            strcpy (xserverrcbuf, cp);
            result = True;
        } else if ( xdg_config != NULL )
            snprintf (xserverrcbuf, sizeof (xserverrcbuf), "%s%s", xdg_config, xserverrc);
        else if ( home != NULL )
            snprintf (xserverrcbuf, sizeof (xserverrcbuf), "%s/.config%s", home, xserverrc);
        
        if ( *xserverrcbuf != '\0' ) {
            if ( access (xserverrcbuf, R_OK) == 0 ) {
                server += start_of_server_args - 1;
                *server = xserverrcbuf;
            } else if ( result )
                error("warning, no server init file \"%s\"", xserverrcbuf);
        }
    }
    /*
     * Check execute permissions
     */

    if ( !check_execute_rights (*server) )
        goto quit;

    return 0;

    /*
     * Start the server and client.
     */
    signal (SIGCHLD, SIG_DFL);    /* Insurance */

    /* Let those signal interrupt the wait() call in the main loop */
    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = sigCatch;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = 0;    /* do not set SA_RESTART */

    sigaction (SIGTERM, &sa, NULL);
    sigaction (SIGQUIT, &sa, NULL);
    sigaction (SIGINT, &sa, NULL);
    sigaction (SIGHUP, &sa, NULL);
    sigaction (SIGPIPE, &sa, NULL);

    memset (&si, 0, sizeof (si));
    si.sa_handler = sigIgnore;
    sigemptyset (&si.sa_mask);
    si.sa_flags = SA_RESTART;

    sigaction (SIGALRM, &si, NULL);
    sigaction (SIGUSR1, &si, NULL);

#ifdef __APPLE__
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
    vt = vproc_transaction_begin (NULL);
#endif
#endif

    euid = geteuid ();
    if ( startServer (server, uid != euid) == -1 )
        goto quit;

    if ( startClient (client, euid, uid) == -1 )
        goto quit;

    pid = -1;
    while ( pid != clientpid && pid != serverpid && gotSignal == 0 ) {
        pid = wait (NULL);
    }

#ifdef __APPLE__
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
    vproc_transaction_end (NULL, vt);
#endif
#endif

    signal (SIGTERM, SIG_IGN);
    signal (SIGQUIT, SIG_IGN);
    signal (SIGINT, SIG_IGN);
    signal (SIGHUP, SIG_IGN);
    signal (SIGPIPE, SIG_IGN);

    if ( !shutdown () )
        goto quit;

    if ( gotSignal != 0 ) {
        errorx ("unexpected signal %d", gotSignal);
        goto quit;
    }
    if ( serverpid < 0 ) {
        errorx ("server error");
        goto quit;
    }
    if ( clientpid < 0 ) {
        errorx ("client error");
        goto quit;
    }
    return EXIT_SUCCESS;

quit:

    free_util ();
    return EXIT_FAILURE;
}


/*
 *    waitforserver - wait for X server to start up
 */
static Bool
waitforserver(void)
{
    int ncycles = 120;        /* # of cycles to wait */
    int cycles;            /* Wait cycle count */

#ifdef __APPLE__
    /* For Apple, we don't get signaled by the server when it's ready, so we just
     * want to sleep now since we're going to sleep later anyways and this allows us
     * to avoid the awkard, "why is there an error message in the log" questions
     * from users.
     */

    sleep(2);
#endif

    for ( cycles = 0; cycles < ncycles; cycles++ ) {
        xd = XOpenDisplay (u_display);
        if ( xd != NULL )
            return True;
        
        if ( !processTimeout (1, "X server to begin accepting connections") )
            break;
    }
    errorx ("giving up");
    return False;
}

/*
 * return True if we timeout waiting for pid to exit, False otherwise.
 */
static Bool
processTimeout (int timeout, const char *string)
{
    int i = 0;
    pid_t pidfound = -1;
    static const char *laststring = NULL;

    for ( ;; ) {
        pidfound = waitpid(serverpid, &status, WNOHANG);
        if ( pidfound == serverpid )
            break;

        if (timeout) {
            if ( i == 0 && string != laststring ) {
                /* 'string' should not be null, but let's make the
                 * code safe enough */
                if ( string != NULL )
                    fprintf (stderr, "\r\nwaiting for %s ", string);
                else {
                    fputc ('\r', stderr);
                    fputc ('\n', stderr);
                }
            }
            else
                fputc ('.', stderr);

            fflush(stderr);
            sleep(1);
        }

        if ( ++i > timeout )
            break;
    }

    if ( i > 0 )
        fputc ('\n', stderr);     /* tidy up after message */

    laststring = string;
    return serverpid != pidfound;
}

static pid_t
startServer (char *server_argv[], Bool elevated_rights)
{
    sigset_t mask, old;
    const char * const *cpp;

    debugx ("starting server %s", server_argv[0]);

    sigemptyset (&mask);
    sigaddset (&mask, SIGUSR1);
    sigprocmask (SIG_BLOCK, &mask, &old);

    serverpid = fork ();
    debugx ("server forked: pid=%d", serverpid);
    
    switch (serverpid) {
    case 0:
        /* Unblock */
        sigprocmask (SIG_SETMASK, &old, NULL);

        /*
         * don't hang on read/write to control tty
         */
        signal (SIGTTIN, SIG_IGN);
        signal (SIGTTOU, SIG_IGN);
        /*
         * ignore SIGUSR1 in child.  The server
         * will notice this and send SIGUSR1 back
         * at xinit when ready to accept connections
         */
        signal (SIGUSR1, SIG_IGN);
        /*
         * prevent server from getting sighup from vhangup()
         * if client is xterm -L
         */
        setpgid (0, getpid());
        ExecuteXorg (server_argv, elevated_rights);

        error ("unable to run server \"%s\"", *server_argv);
        fprintf (stderr, "Use the -- option, or make sure that \"%s\" is a program or a link to the right type of server for your display.  Possible server names include:\n", *server_argv);

        for ( cpp = server_names; *cpp; cpp++ )
            fprintf (stderr, "    %s\n", *cpp);

        fprintf (stderr, "\n");
        return -1;
 
    case -1:
        break;
 
    default:
        /*
         * don't nice server
         */
        setpriority (PRIO_PROCESS, serverpid, -1);

        errno = 0;
        if ( !processTimeout (0, NULL) ) {
            serverpid = -1;
            break;
        }
        /*
         * kludge to avoid race with TCP, giving server time to
         * set his socket options before we try to open it,
         * either use the 15 second timeout, or await SIGUSR1.
         *
         * If your machine is substantially slower than 15 seconds,
         * you can easily adjust this value.
         */
        alarm (15);

        sigsuspend (&old);
        alarm (0);
        sigprocmask (SIG_SETMASK, &old, NULL);

        if ( waitforserver () == 0 ) {
            error ("unable to connect to X server");
            shutdown ();
            serverpid = -1;
            return -1;
        }
        break;
    }
    return serverpid;
}

static void
setWindowPath (void)
{
    /* setting WINDOWPATH for clients */
    Atom prop;
    Atom actualtype;
    int actualformat;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *buf;
    const char *windowpath;
    char *newwindowpath;
    unsigned long num;
    char nums [10];
    int numn;
    size_t len;

    debugx ("setting window path");

    prop = XInternAtom(xd, "XFree86_VT", False);
    if (prop == None) {
        errorx("unable to intern XFree86_VT atom");
        return;
    }
    if (XGetWindowProperty(xd, DefaultRootWindow(xd), prop, 0, 1,
        False, AnyPropertyType, &actualtype, &actualformat,
        &nitems, &bytes_after, &buf)) {
        errorx("no XFree86_VT property detected on X server, WINDOWPATH won't be set");
        return;
    }
    if (nitems != 1) {
        errorx("XFree86_VT property unexpectedly has %lu items instead of 1", nitems);
        XFree(buf);
        return;
    }

    switch (actualtype) {
    case XA_CARDINAL:
    case XA_INTEGER:
    case XA_WINDOW:
        switch (actualformat) {
        case  8:
            num = (*(uint8_t  *)(void *)buf);
            break;
        case 16:
            num = (*(uint16_t *)(void *)buf);
            break;
        case 32:
            num = (*(uint32_t *)(void *)buf);
            break;
        default:
            errorx ("XFree86_VT property has unexpected format %d", actualformat);
            XFree (buf);
            return;
        }
        break;

    default:
        errorx ("XFree86_VT property has unexpected type %lx", actualtype);
        XFree (buf);
        return;
    }

    XFree (buf);
    windowpath = getenv ("WINDOWPATH");
    numn = snprintf (nums, sizeof (nums), "%lu", num);

    if (!windowpath) {
        len = numn + 1;
        newwindowpath = x_malloc (len);
        if ( newwindowpath == NULL )
            return;

        snprintf(newwindowpath, len, "%s", nums);
    } else {
        len = strlen (windowpath) + 1 + numn + 1;
        newwindowpath = x_malloc (len);
        if ( newwindowpath == NULL )
            return;

        snprintf (newwindowpath, len, "%s:%s", windowpath, nums);
    }

    if (setenv ("WINDOWPATH", newwindowpath, True) == -1)
        error ("unable to set WINDOWPATH");

    free (newwindowpath);
}

static pid_t
startClient (char *client_argv[], uid_t euid, uid_t uid)
{
    debugx ("starting client %s: euid=%d, uid=%d", client_argv[0], euid, uid);

    /* We don't want to launch the client with elevated rights,
     * so drop setuid and setgid permissions */
    if ( euid != uid && !drop_user_privileges (uid) )
        return -1;

    /* Elevated user id should be the same with real user id */
    euid = geteuid();
    clientpid = fork ();
    debugx ("client forked: pid=%d, euid=%d", clientpid, euid);

    if ( clientpid != 0 )
        return clientpid;

    if ( !set_display_env () )
        return -1;

    setWindowPath ();

    if ( setuid (uid) == -1 ) {
        error ("cannot change uid");
        return -1;
    }
    
    setpgid (0, getpid());
    ExecuteRelative (client_argv);
   
    error ("unable to run program \"%s\". Specify a program on the command line", client_argv[0]);
    return -1;
}

static jmp_buf close_env;

static int
ignorexio (Display *dpy)
{
    errorx ("connection to X server lost");
    longjmp (close_env, 1);
    
    /* NOTREACHED */
    return 0;
}

static Bool
shutdown(void)
{
    debugx ("shutdown: clientpid=%d, serverid=%d", clientpid, serverpid);

    /* have kept display opened, so close it now */
    if ( clientpid > 0 ) {
        XSetIOErrorHandler (ignorexio);

        if ( !setjmp (close_env) )
            XCloseDisplay(xd);

        /* HUP all local clients to allow them to clean up */
        if (killpg (clientpid, SIGHUP) < 0 && errno != ESRCH)
            error ("can't send HUP to process group %d", clientpid);
    }

    if ( serverpid < 0 )
        return True;

    if (killpg (serverpid, SIGTERM) < 0) {
        if (errno == ESRCH)
            return True;

        error ("can't kill X server");
        return False;
    }

    if (!processTimeout (10, "X server to shut down"))
        return True;

    errorx ("X server slow to shut down, sending KILL signal");

    if (killpg (serverpid, SIGKILL) < 0) {
        if (errno == ESRCH)
            return True;

        error ("can't SIGKILL X server");
    }

    if (processTimeout (3, "server to die")) {
        errorx ("X server refuses to die");
        return False;
    }
#ifdef __sun
    else {
        /* Restore keyboard mode. */
        serverpid = fork ();
        
        switch (serverpid) {
        case 0:
            execl (kbd_mode, kbd_mode, "-a", NULL);
            error ("unable to run program \"%s\"", kbd_mode);
            return False;

        case -1:
            error ("fork failed");
            break;

        default:
            fprintf (stderr, "\r\nRestoring keyboard mode\r\n");
            processTimeout (1, kbd_mode);
        }
    }
#endif /* __sun */

    return True;
}
