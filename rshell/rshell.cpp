#include <iostream>
#include <poll.h>
#include <errno.h>
#include <pty.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <cstdio>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

const int BACKLOG = 64;

int safe_open(const char * path, int flags)
{
    int fd = open(path, flags);

    if (fd == -1)
    {
        //        std::cerr << "IO error\n";
        _exit(20);
    }

    return fd;
}

void write_all(int fd, const char * buf, int count)
{
    int written;

    for (written = 0; written < count;)
    {
        int res = write(fd, buf + written, count - written);
        if (res < 0) return;
        written += res;
    }
}

void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, O_NONBLOCK | flags);
}

bool exchange_data(int fd1, int fd2, char * buf, int buf_size)
{
    int r1 = read(fd1, buf, buf_size);
    if (r1 > 0)
        write_all(fd2, buf, r1);
    else if (r1 == 0)
        return false;
    else if (r1 == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
    return true;
}

pid_t pid;

void handler(int) {
    kill(pid, SIGINT);
}


int main(int, char **)
{
    pid = fork();
    if (pid)
    {
        signal(SIGINT, handler);
        std::cout << "Started daemon, pid: " << pid << std::endl;
        int status;
        waitpid(pid, &status, 0);
        std::cout << "Daemon stopped" << std::endl;
    } else
    {
        int log_fd = dup(STDOUT_FILENO);
        if (log_fd == -1)
        {
            _exit(43);
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        setsid();

        addrinfo hints;
        memset(&hints, 0, sizeof(addrinfo));

        // Setting connection options
        hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
        hints.ai_socktype = SOCK_STREAM; /* TCP socket */
        hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
        hints.ai_protocol = 0;          /* Any protocol */

        // Getting address info
        addrinfo *result;
        {
            int ainfo = getaddrinfo(NULL, "8822", &hints, &result);
            if (ainfo != 0)
            {
                _exit(EXIT_FAILURE);
            }
        }

        // Creating socket
        int sockfd = socket(result->ai_family, result->ai_socktype,
                            result->ai_protocol);

        if (sockfd == -1)
            _exit(EXIT_FAILURE);

        // Setting socket option to reuse address
        {
            int optval = 1;
            int ss = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
            if (ss != 0)
            {
                _exit(EXIT_FAILURE);
            }
        }

        // Binding socket to the first network interface
        if (bind(sockfd, result->ai_addr, result->ai_addrlen))
        {
            _exit(EXIT_FAILURE);
        }

        // Listening address
        if (listen(sockfd, BACKLOG))
        {
            _exit(EXIT_FAILURE);
        }

        while (true)
        {
            // Waiting for somebody to connect
            int fd = accept(sockfd, result->ai_addr, &result->ai_addrlen);
            const char msg[] = "Connection received\n";
            write_all(log_fd, msg, strlen(msg));

            if (fork())
            {
                // Preparing to accept next connection in daemon
                close(fd);
            } else
            {
                // Opening pty for remote shell
                int master, slave;
                char name[4096];
                int pty_res = openpty(&master, &slave, name, NULL, NULL);
                if (pty_res == -1)
                {
                    _exit(100);
                }

                if (fork())
                {
                    // Stupid exchange of data between client and terminal
                    close(slave);
//                    set_nonblock(master);
//                    set_nonblock(fd);

                    const int BUF_SIZE = 4096;
                    char buf[BUF_SIZE];

                    const int nfds = 2;
                    pollfd fds[nfds];
                    fds[0].fd = master;
                    fds[1].fd = fd;

                    const short ERR_CODES = POLLERR | POLLHUP | POLLNVAL | POLLRDHUP;

                    fds[0].events = fds[1].events = POLLIN | ERR_CODES;
                    while (true)
                    {
                        int res = poll(fds, nfds, -1);
                        if (res < 0)
                        {
                            _exit(42);
                        }

                        if ((fds[0].revents & ERR_CODES) || (fds[1].revents & ERR_CODES))
                        {
                            break;
                        }

                        if (fds[0].revents & POLLIN)
                        {
                            exchange_data(master, fd, buf, BUF_SIZE);
                        }

                        if (fds[1].revents & POLLIN)
                        {
                            exchange_data(fd, master, buf, BUF_SIZE);
                        }
                    }
                    close(master);
                    close(fd);
                    const char msg[] = "Connection closed\n";
                    write_all(log_fd, msg, strlen(msg));
                    close(log_fd);
                    _exit(0);
                } else
                {
                    // Creating a session and running shell in it
                    close(master);
                    close(fd);
                    close(log_fd);
                    setsid();

                    int pty_fd = safe_open(name, O_RDWR);
                    close(pty_fd);

                    dup2(slave, STDIN_FILENO);
                    dup2(slave, STDOUT_FILENO);
                    dup2(slave, STDERR_FILENO);

                    close(slave);

                    execlp("/bin/sh", "sh", NULL);
                    _exit(6);
                }
            }
        }
    }
}

