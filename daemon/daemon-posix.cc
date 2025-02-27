// This file Copyright © 2015-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h> /* abort(), daemon(), exit() */
#include <fcntl.h> /* open() */
#include <unistd.h> /* fork(), setsid(), chdir(), dup2(), close(), pipe() */

#include <libtransmission/transmission.h>
#include <libtransmission/error.h>
#include <libtransmission/utils.h>

#include "daemon.h"

using namespace std::literals;

/***
****
***/

static dtr_callbacks const* callbacks = nullptr;
static void* callback_arg = nullptr;

static int signal_pipe[2];

/***
****
***/

static void set_system_error(tr_error** error, int code, std::string_view message)
{
    tr_error_set(error, code, tr_strvJoin(message, " ("sv, std::to_string(code), "): "sv, tr_strerror(code)));
}

/***
****
***/

static void handle_signal(int sig)
{
    switch (sig)
    {
    case SIGHUP:
        callbacks->on_reconfigure(callback_arg);
        break;

    case SIGINT:
    case SIGTERM:
        callbacks->on_stop(callback_arg);
        break;

    default:
        assert("Unexpected signal");
    }
}

static void send_signal_to_pipe(int sig)
{
    int const old_errno = errno;

    if (write(signal_pipe[1], &sig, sizeof(sig)) == -1)
    {
        abort();
    }

    errno = old_errno;
}

static void* signal_handler_thread_main(void* /*arg*/)
{
    int sig;

    while (read(signal_pipe[0], &sig, sizeof(sig)) == sizeof(sig) && sig != 0)
    {
        handle_signal(sig);
    }

    return nullptr;
}

static bool create_signal_pipe(tr_error** error)
{
    if (pipe(signal_pipe) == -1)
    {
        set_system_error(error, errno, "pipe() failed");
        return false;
    }

    return true;
}

static void destroy_signal_pipe(void)
{
    close(signal_pipe[0]);
    close(signal_pipe[1]);
}

static bool create_signal_handler_thread(pthread_t* thread, tr_error** error)
{
    if (!create_signal_pipe(error))
    {
        return false;
    }

    if ((errno = pthread_create(thread, nullptr, &signal_handler_thread_main, nullptr)) != 0)
    {
        set_system_error(error, errno, "pthread_create() failed");
        destroy_signal_pipe();
        return false;
    }

    return true;
}

static void destroy_signal_handler_thread(pthread_t thread)
{
    send_signal_to_pipe(0);
    pthread_join(thread, nullptr);

    destroy_signal_pipe();
}

static bool setup_signal_handler(int sig, tr_error** error)
{
    assert(sig != 0);

    if (signal(sig, &send_signal_to_pipe) == SIG_ERR)
    {
        set_system_error(error, errno, "signal() failed");
        return false;
    }

    return true;
}

/***
****
***/

bool dtr_daemon(dtr_callbacks const* cb, void* cb_arg, bool foreground, int* exit_code, tr_error** error)
{
    callbacks = cb;
    callback_arg = cb_arg;

    *exit_code = 1;

    if (!foreground)
    {
#if defined(HAVE_DAEMON) && !defined(__APPLE__) && !defined(__UCLIBC__)

        if (daemon(true, false) == -1)
        {
            set_system_error(error, errno, "daemon() failed");
            return false;
        }

#else

        /* this is loosely based off of glibc's daemon() implementation
         * http://sourceware.org/git/?p=glibc.git;a=blob_plain;f=misc/daemon.c */

        switch (fork())
        {
        case -1:
            set_system_error(error, errno, "fork() failed");
            return false;

        case 0:
            break;

        default:
            *exit_code = 0;
            return true;
        }

        if (setsid() == -1)
        {
            set_system_error(error, errno, "setsid() failed");
            return false;
        }

        /*
        if (chdir("/") == -1)
        {
            set_system_error(error, errno, "chdir() failed");
            return false;
        }
        */

        {
            int const fd = open("/dev/null", O_RDWR, 0);
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

#endif
    }

    pthread_t signal_thread;

    if (!create_signal_handler_thread(&signal_thread, error))
    {
        return false;
    }

    if (!setup_signal_handler(SIGINT, error) || !setup_signal_handler(SIGTERM, error) || !setup_signal_handler(SIGHUP, error))
    {
        destroy_signal_handler_thread(signal_thread);
        return false;
    }

    *exit_code = cb->on_start(cb_arg, foreground);

    destroy_signal_handler_thread(signal_thread);

    return true;
}
