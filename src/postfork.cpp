// Functions that we may safely call after fork().
#include "config.h"  // IWYU pragma: keep

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <memory>
#if FISH_USE_POSIX_SPAWN
#include <spawn.h>
#endif
#include <wchar.h>

#include "common.h"
#include "exec.h"
#include "io.h"
#include "iothread.h"
#include "postfork.h"
#include "proc.h"
#include "signal.h"
#include "wutil.h"  // IWYU pragma: keep

#ifndef JOIN_THREADS_BEFORE_FORK
#define JOIN_THREADS_BEFORE_FORK 0
#endif

/// The number of times to try to call fork() before giving up.
#define FORK_LAPS 5

/// The number of nanoseconds to sleep between attempts to call fork().
#define FORK_SLEEP_TIME 1000000

/// Base open mode to pass to calls to open.
#define OPEN_MASK 0666

/// Fork error message.
#define FORK_ERROR "Could not create child process - exiting"

/// File redirection clobbering error message.
#define NOCLOB_ERROR "The file '%s' already exists"

/// File redirection error message.
#define FILE_ERROR "An error occurred while redirecting file '%s'"

/// File descriptor redirection error message.
#define FD_ERROR "An error occurred while redirecting file descriptor %s"

/// Pipe error message.
#define LOCAL_PIPE_ERROR "An error occurred while setting up pipe"

static bool log_redirections = false;

/// Cover for debug_safe that can take an int. The format string should expect a %s.
static void debug_safe_int(int level, const char *format, int val) {
    char buff[128];
    format_long_safe(buff, val);
    debug_safe(level, format, buff);
}

/// Called only by the child to set its own process group (possibly creating a new group in the
/// process if it is the first in a JOB_CONTROL job. The parent will wait for this to finish.
/// A process that isn't already in control of the terminal can't give itself control of the
/// terminal without hanging, but it's not right for the child to try and give itself control
/// from the very beginning because the parent may not have gotten around to doing so yet. Let
/// the parent figure it out; if the child doesn't have terminal control and it later tries to
/// read from the terminal, the kernel will send it SIGTTIN and it'll hang anyway.
/// The key here is that the parent should transfer control of the terminal (if appropriate)
/// prior to sending the child SIGCONT to wake it up to exec.
///
/// Returns true on sucess, false on failiure.
bool child_set_group(job_t *j, process_t *p) {
    bool retval = true;

    if (j->get_flag(JOB_CONTROL)) {
        // New jobs have the pgid set to -2
        if (j->pgid == -2) {
            j->pgid = p->pid;
        }
        // Retry on EPERM because there's no way that a child cannot join an existing progress group
        // because we are SIGSTOPing the previous job in the chain. Sometimes we have to try a few
        // times to get the kernel to see the new group. (Linux 4.4.0)
        int failure = setpgid(p->pid, j->pgid);
        while (failure == -1 && (errno == EPERM || errno == EINTR)) {
            debug_safe(4, "Retrying setpgid in child process");
            failure = setpgid(p->pid, j->pgid);
        }
        // TODO: Figure out why we're testing whether the pgid is correct after attempting to
        // set it failed. This was added in commit 4e912ef8 from 2012-02-27.
        failure = failure && getpgid(p->pid) != j->pgid;
        if (failure) {  //!OCLINT(collapsible if statements)
            char pid_buff[128];
            char job_id_buff[128];
            char getpgid_buff[128];
            char job_pgid_buff[128];
            char argv0[64];
            char command[64];

            format_long_safe(pid_buff, p->pid);
            format_long_safe(job_id_buff, j->job_id);
            format_long_safe(getpgid_buff, getpgid(p->pid));
            format_long_safe(job_pgid_buff, j->pgid);
            narrow_string_safe(argv0, p->argv0());
            narrow_string_safe(command, j->command_wcstr());

            debug_safe(
                1, "Could not send own process %s, '%s' in job %s, '%s' from group %s to group %s",
                pid_buff, argv0, job_id_buff, command, getpgid_buff, job_pgid_buff);

            safe_perror("setpgid");
            retval = false;
        }
    } else {
        // This is probably stays unused in the child.
        j->pgid = getpgrp();
    }

    return retval;
}

/// Called only by the parent only after a child forks and successfully calls child_set_group,
/// guaranteeing the job control process group has been created and that the child belongs to the
/// correct process group. Here we can update our job_t structure to reflect the correct process
/// group in the case of JOB_CONTROL, and we can give the new process group control of the terminal
/// if it's to run in the foreground. Note that we can guarantee the child won't try to read from
/// the terminal before we've had a chance to run this code, because we haven't woken them up with a
/// SIGCONT yet. This musn't be called as a part of setup_child_process because that can hang
/// indefinitely until data is available to read/write in the case of IO_FILE, which means we'll
/// never reach our SIGSTOP and everything hangs.
bool set_child_group(job_t *j, pid_t child_pid) {
    bool retval = true;

    if (j->get_flag(JOB_CONTROL)) {
        // New jobs have the pgid set to -2
        if (j->pgid == -2) {
            j->pgid = child_pid;
        }
    } else {
        j->pgid = getpgrp();
    }

    if (j->get_flag(JOB_TERMINAL) && j->get_flag(JOB_FOREGROUND)) {  //!OCLINT(early exit)
        if (tcgetpgrp(STDIN_FILENO) == j->pgid) {
            // We've already assigned the process group control of the terminal when the first
            // process in the job was started. There's no need to do so again, and on some platforms
            // this can cause an EPERM error. In addition, if we've given control of the terminal to
            // a process group, attempting to call tcsetpgrp from the background will cause SIGTTOU
            // to be sent to everything in our process group (unless we handle it).
            debug(4, L"Process group %d already has control of terminal\n", j->pgid);
        } else {
            // No need to duplicate the code here, a function already exists that does just this.
            retval = terminal_give_to_job(j, false /*new job, so not continuing*/);
        }
    }

    return retval;
}

/// Set up a childs io redirections. Should only be called by setup_child_process(). Does the
/// following: First it closes any open file descriptors not related to the child by calling
/// close_unused_internal_pipes() and closing the universal variable server file descriptor. It then
/// goes on to perform all the redirections described by \c io.
///
/// \param io_chain the list of IO redirections for the child
///
/// \return 0 on sucess, -1 on failure
static int handle_child_io(const io_chain_t &io_chain) {
    for (size_t idx = 0; idx < io_chain.size(); idx++) {
        const io_data_t *io = io_chain.at(idx).get();

        if (io->io_mode == IO_FD && io->fd == static_cast<const io_fd_t *>(io)->old_fd) {
            continue;
        }

        switch (io->io_mode) {
            case IO_CLOSE: {
                if (log_redirections) fwprintf(stderr, L"%d: close %d\n", getpid(), io->fd);
                if (close(io->fd)) {
                    debug_safe_int(0, "Failed to close file descriptor %s", io->fd);
                    safe_perror("close");
                }
                break;
            }

            case IO_FILE: {
                // Here we definitely do not want to set CLO_EXEC because our child needs access.
                const io_file_t *io_file = static_cast<const io_file_t *>(io);
                int tmp = open(io_file->filename_cstr, io_file->flags, OPEN_MASK);
                if (tmp < 0) {
                    if ((io_file->flags & O_EXCL) && (errno == EEXIST)) {
                        debug_safe(1, NOCLOB_ERROR, io_file->filename_cstr);
                    } else {
                        debug_safe(1, FILE_ERROR, io_file->filename_cstr);
                        safe_perror("open");
                    }

                    return -1;
                } else if (tmp != io->fd) {
                    // This call will sometimes fail, but that is ok, this is just a precausion.
                    close(io->fd);

                    if (dup2(tmp, io->fd) == -1) {
                        debug_safe_int(1, FD_ERROR, io->fd);
                        safe_perror("dup2");
                        exec_close(tmp);
                        return -1;
                    }
                    exec_close(tmp);
                }
                break;
            }

            case IO_FD: {
                int old_fd = static_cast<const io_fd_t *>(io)->old_fd;
                if (log_redirections)
                    fwprintf(stderr, L"%d: fd dup %d to %d\n", getpid(), old_fd, io->fd);

                // This call will sometimes fail, but that is ok, this is just a precausion.
                close(io->fd);

                if (dup2(old_fd, io->fd) == -1) {
                    debug_safe_int(1, FD_ERROR, io->fd);
                    safe_perror("dup2");
                    return -1;
                }
                break;
            }

            case IO_BUFFER:
            case IO_PIPE: {
                const io_pipe_t *io_pipe = static_cast<const io_pipe_t *>(io);
                // If write_pipe_idx is 0, it means we're connecting to the read end (first pipe
                // fd). If it's 1, we're connecting to the write end (second pipe fd).
                unsigned int write_pipe_idx = (io_pipe->is_input ? 0 : 1);
#if 0
                debug(0, L"%ls %ls on fd %d (%d %d)", write_pipe?L"write":L"read",
                      (io->io_mode == IO_BUFFER)?L"buffer":L"pipe", io->fd, io->pipe_fd[0],
                      io->pipe_fd[1]);
#endif
                if (log_redirections)
                    fwprintf(stderr, L"%d: %s dup %d to %d\n", getpid(),
                             io->io_mode == IO_BUFFER ? "buffer" : "pipe",
                             io_pipe->pipe_fd[write_pipe_idx], io->fd);
                if (dup2(io_pipe->pipe_fd[write_pipe_idx], io->fd) != io->fd) {
                    debug_safe(1, LOCAL_PIPE_ERROR);
                    safe_perror("dup2");
                    return -1;
                }

                if (io_pipe->pipe_fd[0] >= 0) exec_close(io_pipe->pipe_fd[0]);
                if (io_pipe->pipe_fd[1] >= 0) exec_close(io_pipe->pipe_fd[1]);
                break;
            }
        }
    }

    return 0;
}

int setup_child_process(process_t *p, const io_chain_t &io_chain) {
    bool ok = true;

    if (ok) {
        // In the case of IO_FILE, this can hang until data is available to read/write!
        ok = (0 == handle_child_io(io_chain));
        if (p != 0 && !ok) {
            debug_safe(4, "handle_child_io failed in setup_child_process");
            exit_without_destructors(1);
        }
    }

    if (ok) {
        // Set the handling for job control signals back to the default.
        signal_reset_handlers();
    }

    return ok ? 0 : -1;
}

int g_fork_count = 0;

/// This function is a wrapper around fork. If the fork calls fails with EAGAIN, it is retried
/// FORK_LAPS times, with a very slight delay between each lap. If fork fails even then, the process
/// will exit with an error message.
pid_t execute_fork(bool wait_for_threads_to_die) {
    ASSERT_IS_MAIN_THREAD();

    if (wait_for_threads_to_die || JOIN_THREADS_BEFORE_FORK) {
        // Make sure we have no outstanding threads before we fork. This is a pretty sketchy thing
        // to do here, both because exec.cpp shouldn't have to know about iothreads, and because the
        // completion handlers may do unexpected things.
        debug_safe(4, "waiting for threads to drain.");
        iothread_drain_all();
    }

    pid_t pid;
    struct timespec pollint;
    int i;

    g_fork_count++;

    for (i = 0; i < FORK_LAPS; i++) {
        pid = fork();
        if (pid >= 0) {
            return pid;
        }

        if (errno != EAGAIN) {
            break;
        }

        pollint.tv_sec = 0;
        pollint.tv_nsec = FORK_SLEEP_TIME;

        // Don't sleep on the final lap - sleeping might change the value of errno, which will break
        // the error reporting below.
        if (i != FORK_LAPS - 1) {
            nanosleep(&pollint, NULL);
        }
    }

    debug_safe(0, FORK_ERROR);
    safe_perror("fork");
    FATAL_EXIT();
    return 0;
}

#if FISH_USE_POSIX_SPAWN
bool fork_actions_make_spawn_properties(posix_spawnattr_t *attr,
                                        posix_spawn_file_actions_t *actions, job_t *j, process_t *p,
                                        const io_chain_t &io_chain) {
    UNUSED(p);
    // Initialize the output.
    if (posix_spawnattr_init(attr) != 0) {
        return false;
    }

    if (posix_spawn_file_actions_init(actions) != 0) {
        posix_spawnattr_destroy(attr);
        return false;
    }

    bool should_set_process_group_id = false;
    int desired_process_group_id = 0;
    if (j->get_flag(JOB_CONTROL)) {
        should_set_process_group_id = true;

        // set_child_group puts each job into its own process group
        // do the same here if there is no PGID yet (i.e. PGID == -2)
        desired_process_group_id = j->pgid;
        if (desired_process_group_id == -2) {
            desired_process_group_id = 0;
        }
    }

    // Set the handling for job control signals back to the default.
    bool reset_signal_handlers = true;

    // Remove all signal blocks.
    bool reset_sigmask = true;

    // Set our flags.
    short flags = 0;
    if (reset_signal_handlers) flags |= POSIX_SPAWN_SETSIGDEF;
    if (reset_sigmask) flags |= POSIX_SPAWN_SETSIGMASK;
    if (should_set_process_group_id) flags |= POSIX_SPAWN_SETPGROUP;

    int err = 0;
    if (!err) err = posix_spawnattr_setflags(attr, flags);

    if (!err && should_set_process_group_id)
        err = posix_spawnattr_setpgroup(attr, desired_process_group_id);

    // Everybody gets default handlers.
    if (!err && reset_signal_handlers) {
        sigset_t sigdefault;
        get_signals_with_handlers(&sigdefault);
        err = posix_spawnattr_setsigdefault(attr, &sigdefault);
    }

    // No signals blocked.
    sigset_t sigmask;
    sigemptyset(&sigmask);
    if (!err && reset_sigmask) err = posix_spawnattr_setsigmask(attr, &sigmask);

    for (size_t idx = 0; idx < io_chain.size(); idx++) {
        const shared_ptr<const io_data_t> io = io_chain.at(idx);

        if (io->io_mode == IO_FD) {
            const io_fd_t *io_fd = static_cast<const io_fd_t *>(io.get());
            if (io->fd == io_fd->old_fd) continue;
        }

        switch (io->io_mode) {
            case IO_CLOSE: {
                if (!err) err = posix_spawn_file_actions_addclose(actions, io->fd);
                break;
            }

            case IO_FILE: {
                const io_file_t *io_file = static_cast<const io_file_t *>(io.get());
                if (!err)
                    err = posix_spawn_file_actions_addopen(actions, io->fd, io_file->filename_cstr,
                                                           io_file->flags /* mode */, OPEN_MASK);
                break;
            }

            case IO_FD: {
                const io_fd_t *io_fd = static_cast<const io_fd_t *>(io.get());
                if (!err)
                    err = posix_spawn_file_actions_adddup2(actions, io_fd->old_fd /* from */,
                                                           io->fd /* to */);
                break;
            }

            case IO_BUFFER:
            case IO_PIPE: {
                const io_pipe_t *io_pipe = static_cast<const io_pipe_t *>(io.get());
                unsigned int write_pipe_idx = (io_pipe->is_input ? 0 : 1);
                int from_fd = io_pipe->pipe_fd[write_pipe_idx];
                int to_fd = io->fd;
                if (!err) err = posix_spawn_file_actions_adddup2(actions, from_fd, to_fd);

                if (write_pipe_idx > 0) {
                    if (!err) err = posix_spawn_file_actions_addclose(actions, io_pipe->pipe_fd[0]);
                    if (!err) err = posix_spawn_file_actions_addclose(actions, io_pipe->pipe_fd[1]);
                } else {
                    if (!err) err = posix_spawn_file_actions_addclose(actions, io_pipe->pipe_fd[0]);
                }
                break;
            }
        }
    }

    // Clean up on error.
    if (err) {
        posix_spawnattr_destroy(attr);
        posix_spawn_file_actions_destroy(actions);
    }

    return !err;
}
#endif  // FISH_USE_POSIX_SPAWN

void safe_report_exec_error(int err, const char *actual_cmd, const char *const *argv,
                            const char *const *envv) {
    debug_safe(0, "Failed to execute process '%s'. Reason:", actual_cmd);

    switch (err) {
        case E2BIG: {
            char sz1[128], sz2[128];

            long arg_max = -1;

            size_t sz = 0;
            const char *const *p;
            for (p = argv; *p; p++) {
                sz += strlen(*p) + 1;
            }

            for (p = envv; *p; p++) {
                sz += strlen(*p) + 1;
            }

            format_size_safe(sz1, sz);
            arg_max = sysconf(_SC_ARG_MAX);

            if (arg_max > 0) {
                format_size_safe(sz2, static_cast<unsigned long long>(arg_max));
                debug_safe(0,
                           "The total size of the argument and environment lists %s exceeds the "
                           "operating system limit of %s.",
                           sz1, sz2);
            } else {
                debug_safe(0,
                           "The total size of the argument and environment lists (%s) exceeds the "
                           "operating system limit.",
                           sz1);
            }

            debug_safe(0, "Try running the command again with fewer arguments.");
            break;
        }

        case ENOEXEC: {
            const char *err = safe_strerror(errno);
            debug_safe(0, "exec: %s", err);

            debug_safe(0,
                       "The file '%s' is marked as an executable but could not be run by the "
                       "operating system.",
                       actual_cmd);
            break;
        }

        case ENOENT: {
            // ENOENT is returned by exec() when the path fails, but also returned by posix_spawn if
            // an open file action fails. These cases appear to be impossible to distinguish. We
            // address this by not using posix_spawn for file redirections, so all the ENOENTs we
            // find must be errors from exec().
            char interpreter_buff[128] = {}, *interpreter;
            interpreter = get_interpreter(actual_cmd, interpreter_buff, sizeof interpreter_buff);
            if (interpreter && 0 != access(interpreter, X_OK)) {
                debug_safe(0,
                           "The file '%s' specified the interpreter '%s', which is not an "
                           "executable command.",
                           actual_cmd, interpreter);
            } else {
                debug_safe(0, "The file '%s' does not exist or could not be executed.", actual_cmd);
            }
            break;
        }

        case ENOMEM: {
            debug_safe(0, "Out of memory");
            break;
        }

        default: {
            const char *err = safe_strerror(errno);
            debug_safe(0, "exec: %s", err);

            // debug(0, L"The file '%ls' is marked as an executable but could not be run by the
            // operating system.", p->actual_cmd);
            break;
        }
    }
}

/// Perform output from builtins. May be called from a forked child, so don't do anything that may
/// allocate memory, etc.
bool do_builtin_io(const char *out, size_t outlen, const char *err, size_t errlen) {
    int saved_errno = 0;
    bool success = true;
    if (out && outlen && write_loop(STDOUT_FILENO, out, outlen) < 0) {
        saved_errno = errno;
        if (errno != EPIPE) {
            debug_safe(0, "Error while writing to stdout");
            errno = saved_errno;
            safe_perror("write_loop");
        }
        success = false;
    }

    if (err && errlen && write_loop(STDERR_FILENO, err, errlen) < 0) {
        saved_errno = errno;
        success = false;
    }

    errno = saved_errno;
    return success;
}
