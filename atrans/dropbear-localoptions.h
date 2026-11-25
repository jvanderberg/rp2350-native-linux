/* Dropbear for ATRANS (BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE):
 * public-key logins only, and no re-exec per connection. Re-exec (fexecve of
 * /proc/self/exe) only re-randomises the address space, which ATRANS
 * processes don't have, and costs a full exec of the 290 KB binary for
 * every login. */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_REEXEC 0
/* X11 forwarding (ssh -X/-Y): a login gets DISPLAY=localhost:10.0, and
 * /usr/bin/xauth (xauth.c) stores the cookie the X client must send. */
#define DROPBEAR_X11FWD 1
