#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 5000
#define MAXLINE 4096

static const char *token;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

static void say(const char *text, size_t n)
{
    if (!n)
        return;
    if (token)
    {
        size_t t = strlen(token);
        if (n < t + 1 || memcmp(text, token, t) || text[t] != ' ')
        {
            printf("rejected: bad/missing token\n");
            return;
        }
        text += t + 1;
        n -= t + 1;
    }
    printf("speaking %zu bytes\n", n);
    FILE *p = popen("espeak", "w");
    if (!p)
    {
        perror("popen");
        return;
    }
    fwrite(text, 1, n, p);
    pclose(p);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    token = getenv("ESPEAK_TOKEN");

    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0)
        die("socket");

    int on = 1, off = 0;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(PORT);
    a.sin6_addr = in6addr_any;

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        die("bind");
    if (listen(s, 16) < 0)
        die("listen");
    printf("Listening on [::]:%d (IPv4 + IPv6)%s\n", PORT, token ? " [token required]" : "");
    fflush(stdout);

    for (;;)
    {
        struct sockaddr_in6 peer;
        socklen_t plen = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &plen);
        if (c < 0)
        {
            perror("accept");
            continue;
        }

        char host[INET6_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET6, &peer.sin6_addr, host, sizeof(host));
        printf("connection from %s\n", host);
        fflush(stdout);

        char buf[MAXLINE];
        size_t len = 0;
        ssize_t n;
        while ((n = read(c, buf + len, sizeof(buf) - len)) > 0)
        {
            len += (size_t)n;
            char *nl;
            while ((nl = memchr(buf, '\n', len)))
            {
                size_t line = (size_t)(nl - buf);
                say(buf, line);
                memmove(buf, nl + 1, len - line - 1);
                len -= line + 1;
            }
            if (len == sizeof(buf))
            {
                say(buf, len);
                len = 0;
            }
        }
        say(buf, len);
        close(c);
        fflush(stdout);
    }
}
