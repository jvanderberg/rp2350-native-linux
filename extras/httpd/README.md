# httpd - a small static file server

    ./build.sh
    ./httpd [-p PORT] [-q] [DIR]    (default: port 8080, the current directory)

It serves the files under DIR. A directory serves its `index.html` if it
has one, otherwise a listing. `-q` stops the log of requests.

- One client at a time, and no fork (the board has no MMU).
- GET and HEAD only.
- Any path part starting with `.` is refused, which blocks both `..`
  escapes and dotfiles such as `.ssh`.
- A client that stalls for 10 seconds is dropped.

See the top of `httpd.c` for more.
