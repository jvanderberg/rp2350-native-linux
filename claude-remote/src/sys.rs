//! Linux system calls, 64-bit time only: 32-bit Arm EABI for an ATRANS
//! program on native RP2350 Linux (thumbv7m, linked at 0x11020000 by
//! link-arm.x).

use alloc::vec::Vec;
use core::arch::asm;

pub const AT_FDCWD: i32 = -100;
pub const AT_REMOVEDIR: u32 = 0x200;
pub const O_RDONLY: u32 = 0;
pub const O_WRONLY: u32 = 1;
pub const O_CREAT: u32 = 0o100;
pub const O_TRUNC: u32 = 0o1000;
// Arm's own value: the asm-generic 0o200000 is O_DIRECT here, and a
// directory opened with it fails with EINVAL
pub const O_DIRECTORY: u32 = 0o40000;
pub const O_CLOEXEC: u32 = 0o2000000;
pub const S_IFMT: u16 = 0xF000;
pub const S_IFDIR: u16 = 0x4000;
pub const SIGKILL: i32 = 9;
pub const SIGCHLD: usize = 17;
pub const POLLIN: i16 = 1;

mod nr {
    pub const GETCWD: usize = 183;
    pub const DUP3: usize = 358;
    pub const MKDIRAT: usize = 323;
    pub const UNLINKAT: usize = 328;
    pub const CHDIR: usize = 12;
    pub const OPENAT: usize = 322;
    pub const CLOSE: usize = 6;
    pub const PIPE2: usize = 359;
    pub const GETDENTS64: usize = 217;
    pub const READ: usize = 3;
    pub const WRITE: usize = 4;
    pub const EXIT_GROUP: usize = 248;
    pub const WAITID: usize = 280;
    pub const KILL: usize = 37;
    pub const SETPGID: usize = 57;
    pub const MUNMAP: usize = 91;
    pub const CLONE: usize = 120;
    pub const EXECVE: usize = 11;
    pub const MMAP: usize = 192; // mmap2: the offset is in pages (always 0 here)
    pub const MADVISE: usize = 220;
    pub const STATX: usize = 397;
    pub const CLOCK_GETTIME64: usize = 403;
    pub const PPOLL_TIME64: usize = 414;
}
use nr::*;

pub type Result<T> = core::result::Result<T, i32>;

/// Arm EABI: svc 0 with the number in r7. r7 is the Thumb frame pointer, which
/// asm! may not name, so the number goes in through another register.
#[inline(always)]
unsafe fn raw(nr: usize, a0: usize, a1: usize, a2: usize, a3: usize, a4: usize, a5: usize) -> isize {
    let ret: isize;
    unsafe {
        asm!("mov {t}, r7", "mov r7, {n}", "svc 0", "mov r7, {t}",
            n = in(reg) nr, t = out(reg) _,
            inlateout("r0") a0 as isize => ret,
            in("r1") a1, in("r2") a2, in("r3") a3, in("r4") a4, in("r5") a5,
            options(nostack));
    }
    ret
}

fn check(r: isize) -> Result<usize> {
    if (-4095..0).contains(&r) { Err(-r as i32) } else { Ok(r as usize) }
}

macro_rules! sc {
    ($nr:expr $(, $a:expr)*) => {{
        let args = [$($a as usize,)* 0usize, 0, 0, 0, 0, 0];
        check(unsafe { raw($nr, args[0], args[1], args[2], args[3], args[4], args[5]) })
    }};
}

/// A path as the kernel wants it: NUL-terminated.
pub fn cstr(s: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s.as_bytes());
    v.push(0);
    v
}

pub fn read(fd: i32, buf: &mut [u8]) -> Result<usize> {
    sc!(READ, fd, buf.as_mut_ptr(), buf.len())
}

pub fn write_all(fd: i32, mut data: &[u8]) -> Result<()> {
    while !data.is_empty() {
        match sc!(WRITE, fd, data.as_ptr(), data.len()) {
            Ok(0) => return Err(5),
            Ok(n) => data = &data[n..],
            Err(4) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

pub fn openat(path: &str, flags: u32, mode: u32) -> Result<i32> {
    let p = cstr(path);
    sc!(OPENAT, AT_FDCWD, p.as_ptr(), flags, mode).map(|fd| fd as i32)
}

pub fn close(fd: i32) {
    let _ = sc!(CLOSE, fd);
}

/// The file's mode and size, through statx.
pub fn stat(path: &str) -> Result<(u16, u64)> {
    let p = cstr(path);
    let mut buf = [0u8; 256];
    sc!(STATX, AT_FDCWD, p.as_ptr(), 0, 0x7ff, buf.as_mut_ptr())?;
    let mode = u16::from_le_bytes([buf[0x1c], buf[0x1d]]);
    let size = u64::from_le_bytes(buf[0x28..0x30].try_into().unwrap());
    Ok((mode, size))
}

pub fn is_dir(path: &str) -> bool {
    matches!(stat(path), Ok((m, _)) if m & S_IFMT == S_IFDIR)
}

pub fn mkdir(path: &str) -> Result<()> {
    let p = cstr(path);
    sc!(MKDIRAT, AT_FDCWD, p.as_ptr(), 0o755).map(|_| ())
}

pub fn unlink(path: &str, flags: u32) -> Result<()> {
    let p = cstr(path);
    sc!(UNLINKAT, AT_FDCWD, p.as_ptr(), flags).map(|_| ())
}

pub fn chdir(path: &str) -> Result<()> {
    let p = cstr(path);
    sc!(CHDIR, p.as_ptr()).map(|_| ())
}

pub fn getcwd() -> Result<Vec<u8>> {
    let mut buf = alloc::vec![0u8; 4096];
    let n = sc!(GETCWD, buf.as_mut_ptr(), buf.len())?;
    buf.truncate(n.saturating_sub(1));
    Ok(buf)
}

/// Names in a directory, in disk order.
pub fn list_dir(path: &str) -> Result<Vec<Vec<u8>>> {
    let fd = openat(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0)?;
    let mut names = Vec::new();
    let mut buf = alloc::vec![0u8; 4096];
    loop {
        let n = match sc!(GETDENTS64, fd, buf.as_mut_ptr(), buf.len()) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                close(fd);
                return Err(e);
            }
        };
        let mut off = 0;
        while off < n {
            // linux_dirent64: ino u64, off i64, reclen u16, type u8, name...
            let reclen = u16::from_le_bytes([buf[off + 16], buf[off + 17]]) as usize;
            let name = &buf[off + 19..off + reclen];
            let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
            let name = &name[..end];
            if name != b"." && name != b".." {
                names.push(name.to_vec());
            }
            off += reclen;
        }
    }
    close(fd);
    Ok(names)
}

pub fn exit(code: i32) -> ! {
    loop {
        let _ = sc!(EXIT_GROUP, code);
    }
}

pub fn mmap(len: usize) -> Result<*mut u8> {
    // PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS
    sc!(MMAP, 0, len, 3, 0x22, -1isize, 0).map(|p| p as *mut u8)
}

pub fn munmap(p: *mut u8, len: usize) {
    let _ = sc!(MUNMAP, p, len);
}

/// MADV_DONTNEED: give the pages back, keep the mapping.
pub fn release(p: *mut u8, len: usize) {
    let _ = sc!(MADVISE, p, len, 4);
}

pub fn pipe() -> Result<(i32, i32)> {
    let mut fds = [0i32; 2];
    sc!(PIPE2, fds.as_mut_ptr(), O_CLOEXEC)?;
    Ok((fds[0], fds[1]))
}

pub fn dup3(old: i32, new: i32) -> Result<()> {
    sc!(DUP3, old, new, 0).map(|_| ())
}

/// fork(): clone with only SIGCHLD, no new stack.
pub fn fork() -> Result<i32> {
    sc!(CLONE, SIGCHLD, 0, 0, 0, 0).map(|p| p as i32)
}

pub fn setpgid(pid: i32, pgid: i32) {
    let _ = sc!(SETPGID, pid, pgid);
}

pub fn execve(path: *const u8, argv: *const *const u8, envp: *const *const u8) -> i32 {
    match sc!(EXECVE, path, argv, envp) {
        Err(e) => e,
        Ok(_) => 0,
    }
}

pub fn kill(pid: i32, sig: i32) {
    let _ = sc!(KILL, pid, sig);
}

/// Waits for the child and reaps it; its status as wait() encodes it (exit
/// code in bits 8-15, or the signal that ended it), through waitid.
pub fn wait(pid: i32) -> i32 {
    const P_PID: usize = 1;
    const WEXITED: usize = 4;
    const CLD_EXITED: i32 = 1;
    // siginfo_t: si_signo, si_errno, si_code, si_pid, si_uid, si_status, ...
    let mut info = [0i32; 32];
    loop {
        match sc!(WAITID, P_PID, pid, info.as_mut_ptr(), WEXITED, 0) {
            Err(4) => {}
            Ok(_) if info[2] == CLD_EXITED => return (info[5] & 0xff) << 8,
            Ok(_) => return info[5] & 0x7f,
            // no such child: report it as killed
            Err(_) => return SIGKILL,
        }
    }
}

/// True when fd is readable before the timeout (ms); false on timeout.
pub fn poll_in(fd: i32, timeout_ms: i64) -> Result<bool> {
    #[repr(C)]
    struct PollFd {
        fd: i32,
        events: i16,
        revents: i16,
    }
    let mut pfd = PollFd { fd, events: POLLIN, revents: 0 };
    let ts: [i64; 2] = [timeout_ms / 1000, (timeout_ms % 1000) * 1_000_000];
    loop {
        match sc!(PPOLL_TIME64, &mut pfd as *mut PollFd, 1, ts.as_ptr(), 0, 8) {
            Ok(0) => return Ok(false),
            Ok(_) => return Ok(true),
            Err(4) => {}
            Err(e) => return Err(e),
        }
    }
}

/// CLOCK_MONOTONIC in milliseconds.
pub fn now_ms() -> i64 {
    let mut ts = [0i64; 2];
    let _ = sc!(CLOCK_GETTIME64, 1, ts.as_mut_ptr());
    ts[0] * 1000 + ts[1] / 1_000_000
}

pub fn errno_text(e: i32) -> &'static str {
    match e {
        1 => "operation not permitted",
        2 => "no such file or directory",
        5 => "I/O error",
        9 => "bad file descriptor",
        12 => "out of memory",
        13 => "permission denied",
        17 => "file exists",
        20 => "not a directory",
        21 => "is a directory",
        22 => "invalid argument",
        24 => "too many open files",
        26 => "text file busy",
        27 => "file too large",
        28 => "no space left on device",
        30 => "read-only file system",
        36 => "file name too long",
        39 => "directory not empty",
        _ => "error",
    }
}
