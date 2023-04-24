/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _UTIL_H
#define _UTIL_H

#define MAX(A, B)  ((A) > (B) ? (A) : (B))

#ifndef True
#define True  1
#endif

#ifndef False
#define False  0
#endif

/* Instead of AUTO which means YES and NO */
#define SCHROEDINGER_CAT  -1
#define DIE  -1

#define countof(a)  (sizeof (a) / sizeof ((a)[0]))


extern const char *prog_name;
extern char *u_session;
extern char *u_display;
extern char *u_server;

void * x_malloc (int size);

const char * s_basename (const char *path);
char * s_dup (const char *s);
void free_util (void);

int set_display_env (void);
int set_session (const char *session);
int set_display (const char *display);
int set_server (const char *server);

char **add_args (char **argv, char *args);

void error_no_memory (void);
void error (const char *fmt, ...);
void errorx (const char *fmt, ...);
void debug (const char *fmt, ...);
void debugx (const char *fmt, ...);

int check_execute_rights (const char *path);
int drop_user_privileges (uid_t uid);
int parse_config (void);
int check_rights (gid_t uid, int shareVTs);
int is_user_allowed (gid_t uid);


#endif  /* _UTIL_H */
