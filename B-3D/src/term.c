#include "b3d.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios savedtty;
static int saved_ok;

void term_detect_size(void)
{
    struct screen *s = &g.scr;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    int fd = open("/dev/tty", O_RDONLY);
    if (fd < 0)
        fd = STDOUT_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col <= 0 || ws.ws_row <= 0)
    {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    if (fd != STDOUT_FILENO)
        close(fd);
    s->cols = s->req_w ? s->req_w : ws.ws_col;
    s->rows = s->req_h ? s->req_h : ws.ws_row;
}

void term_apply_size(void)
{
    struct screen *s = &g.scr;
    s->w = s->cols;
    s->h = s->rows;
    if (g.interactive && g.opt.hud)
        s->h--;
    if (s->h < 4)
        s->h = 4;
    if (s->w < 10)
        s->w = 10;
    render_resize_fb();
    render_update_proj();
}

void term_enter_tui(void)
{
    if (isatty(STDIN_FILENO))
    {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &savedtty) == 0)
        {
            saved_ok = 1;
            t = savedtty;
            t.c_lflag &= ~(ICANON | ECHO);
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
    }
    fputs("\033[?1049h\033[?25l\033[2J", stdout);
    fflush(stdout);
    g.tui = 1;
}

void term_cleanup(void)
{
    if (saved_ok)
        tcsetattr(STDIN_FILENO, TCSANOW, &savedtty);
    if (g.tui)
    {
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        g.tui = 0;
    }
}

static int read_byte(int ms)
{
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    int r = poll(&p, 1, ms);
    if (r == 0)
        return -2;
    if (r < 0)
        return errno == EINTR ? -2 : -1;
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1)
        return c;
    if (n < 0 && errno == EINTR)
        return -2;
    return -1;
}

int term_read_key(char *out, int ms)
{
    int c = read_byte(ms);
    if (c < 0)
        return c;
    out[0] = (char)c;
    if (c != 0x1b)
    {
        out[1] = '\0';
        return 1;
    }
    int c1 = read_byte(10);
    if (c1 < 0)
    {
        out[1] = '\0';
        return 1;
    }
    out[1] = (char)c1;
    out[2] = '\0';
    if (c1 == '[' || c1 == 'O')
    {
        int c2 = read_byte(10);
        if (c2 >= 0)
        {
            out[2] = (char)c2;
            out[3] = '\0';
            int c3 = read_byte(10);
            if (c3 >= 0)
            {
                out[3] = (char)c3;
                out[4] = '\0';
                return 4;
            }
            return 3;
        }
        return 2;
    }
    return 2;
}
