/* 
 * sixwm v2.1 - minimal C tiling window manager for OpenBSD
 * Copyright (c) 2026, swet1shka
 * Licensed under the 2-Clause BSD License.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#define COLOR_ACTIVE   0x81a1c1  // Пастельно-синий (Nord)
#define COLOR_INACTIVE 0x3b4252  // Тёмно-серый для неактивных окон
#define BORDER_WIDTH   3
#define GAPS_SIZE      10

typedef struct Client Client;
struct Client { 
    Window win; 
    Client *next; 
};

Client *clients = NULL;
float mfact = 0.55; // Пропорция главного окна (55% ширины)

// Мгновенная очистка зомби-процессов, адаптированная под строгое ядро OpenBSD
void sigchld_handler(int sig) {
    int saved_errno = errno;
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

// Строгое соответствие типов для системного вызова execvp в OpenBSD
void spawn(char *const cmd[]) {
    if (fork() == 0) {
        if (fork() == 0) {
            setsid();
            execvp(cmd[0], cmd);
        }
        exit(0);
    }
}

void attach(Window win) {
    Client *c = malloc(sizeof(Client));
    if (c) { 
        c->win = win; 
        c->next = clients; 
        clients = c; 
    }
}

void detach(Window win) {
    Client **tc = &clients;
    while (*tc && (*tc)->win != win) tc = &(*tc)->next;
    if (*tc) { 
        Client *d = *tc; 
        *tc = (*tc)->next; 
        free(d); 
    }
}

int count_clients(void) { 
    int n = 0; 
    for (Client *c = clients; c; c = c->next) n++; 
    return n; 
}

// ХОТФИКС ГЕОМЕТРИИ: Защита от схлопывания сетки при большом количестве окон
void tile(Display *dpy) {
    int n = count_clients(); 
    if (n == 0) return;
    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    int sh = DisplayHeight(dpy, DefaultScreen(dpy));

    if (n == 1) {
        XMoveResizeWindow(dpy, clients->win, GAPS_SIZE, GAPS_SIZE, 
                          sw - (GAPS_SIZE * 2) - (BORDER_WIDTH * 2), 
                          sh - (GAPS_SIZE * 2) - (BORDER_WIDTH * 2));
        return;
    }

    int mw = (int)(sw * mfact);
    int stack_wins = n - 1;
    int h = (sh - GAPS_SIZE) / stack_wins;
    int min_h = 20; // Окно не может быть меньше 20 пикселей в высоту
    if (h - GAPS_SIZE - (BORDER_WIDTH * 2) < min_h) {
        h = min_h + GAPS_SIZE + (BORDER_WIDTH * 2);
    }

    int i = 0;
    for (Client *c = clients; c; c = c->next) {
        if (i == 0) { // Мастер-зона (Левое главное окно)
            XMoveResizeWindow(dpy, c->win, GAPS_SIZE, GAPS_SIZE, 
                              mw - GAPS_SIZE - (BORDER_WIDTH * 2), 
                              sh - (GAPS_SIZE * 2) - (BORDER_WIDTH * 2));
        } else { // Стек (Все правые окна)
            int y_pos = GAPS_SIZE + ((i - 1) * h);
            // Если окон больше, чем вмещает экран, плавно уводим их в скрытый буфер
            if (y_pos + h > sh) y_pos = -sh * 2; 
            XMoveResizeWindow(dpy, c->win, mw + GAPS_SIZE, y_pos, 
                              (sw - mw) - (GAPS_SIZE * 2) - (BORDER_WIDTH * 2), 
                              h - GAPS_SIZE - (BORDER_WIDTH * 2));
        }
        i++;
    }
}

// Навигация "Слепой метод" (Alt+J / Alt+K)
void focus(Display *dpy, int dir) {
    if (!clients || !clients->next) return;
    Window fw; 
    int r; 
    XGetInputFocus(dpy, &fw, &r);
    Client *c; 
    for (c = clients; c && c->win != fw; c = c->next);
    Client *nf = NULL;
    if (dir == 1) {
        nf = (c && c->next) ? c->next : clients;
    } else {
        if (c == clients) { 
            for (nf = clients; nf->next; nf = nf->next); 
        } else { 
            for (nf = clients; nf->next != c; nf = nf->next); 
        }
    }
    if (nf) { 
        XSetInputFocus(dpy, nf->win, RevertToParent, CurrentTime); 
        XRaiseWindow(dpy, nf->win); 
        tile(dpy); 
    }
}

// Рокировка окон в мастер-зону (Alt+Space)
void zoom(Display *dpy) {
    Window fw; 
    int r; 
    XGetInputFocus(dpy, &fw, &r);
    if (!clients || clients->win == fw) return;
    detach(fw); 
    attach(fw); 
    tile(dpy);
}

// Безопасное мягкое закрытие окон по протоколу ICCCM
void kill_client(Display *dpy, Window win) {
    Atom *p; 
    int n, s = 0; 
    Atom pd = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    if (XGetWMProtocols(dpy, win, &p, &n)) {
        while (!s && n--) { 
            if (p[n] == pd) s = 1; 
        } 
        XFree(p);
    }
    if (s) {
        XEvent ev; 
        ev.type = ClientMessage; 
        ev.xclient.window = win;
        ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
        ev.xclient.format = 32; 
        ev.xclient.data.l[0] = pd; 
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, win, False, NoEventMask, &ev);
    } else { 
        XKillClient(dpy, win); 
    }
}

int main(void) {
    Display *dpy; 
    Window root; 
    XEvent ev; 
    // Массив строк С для запуска ультра-быстрого терминала st
    char *const termcmd[] = { "st", NULL };
    
    struct sigaction sa; 
    sa.sa_handler = sigchld_handler; 
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; 
    sigaction(SIGCHLD, &sa, NULL);

    if (!(dpy = XOpenDisplay(NULL))) return 1;
    root = DefaultRootWindow(dpy);

    // Регистрация горячих клавиш (Mod1Mask = ALT)
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_Return), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_F4), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_h), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_l), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_j), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_k), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_space), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_r), Mod1Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);

    while (!XNextEvent(dpy, &ev)) {
        if (ev.type == MapRequest) {
            Window w = ev.xmaprequest.window; 
            XWindowAttributes wa; 
            XGetWindowAttributes(dpy, w, &wa);
            if (wa.override_redirect) continue;
            attach(w); 
            XSetWindowBorderWidth(dpy, w, BORDER_WIDTH); 
            XSetWindowBorder(dpy, w, COLOR_ACTIVE);
            XMapWindow(dpy, w); 
            tile(dpy); 
            XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
        }
        if (ev.type == UnmapNotify || ev.type == DestroyNotify) {
            detach((ev.type == UnmapNotify) ? ev.xunmap.window : ev.xdestroywindow.window); 
            tile(dpy);
        }
        if (ev.type == FocusIn) { 
            if (ev.xfocus.window != root && ev.xfocus.window != None) 
                XSetWindowBorder(dpy, ev.xfocus.window, COLOR_ACTIVE); 
        }
        if (ev.type == FocusOut) { 
            if (ev.xfocus.window != root && ev.xfocus.window != None) 
                XSetWindowBorder(dpy, ev.xfocus.window, COLOR_INACTIVE); 
        }
        if (ev.type == KeyPress) {
            KeySym k = XLookupKeysym(&ev.xkey, 0); 
            unsigned int s = ev.xkey.state;
            
            if (k == XK_Return && (s & Mod1Mask)) spawn(termcmd);
            else if (k == XK_F4 && (s & Mod1Mask)) {
                Window fw; int r; 
                XGetInputFocus(dpy, &fw, &r); 
                if (fw != root && fw != None) kill_client(dpy, fw);
            }
            else if (k == XK_h && (s & Mod1Mask)) { if (mfact > 0.10) { mfact -= 0.05; tile(dpy); } }
            else if (k == XK_l && (s & Mod1Mask)) { if (mfact < 0.90) { mfact += 0.05; tile(dpy); } }
            else if (k == XK_j && (s & Mod1Mask)) focus(dpy, 1);
            else if (k == XK_k && (s & Mod1Mask)) focus(dpy, -1);
            else if (k == XK_space && (s & Mod1Mask)) zoom(dpy);
            else if (k == XK_r && (s & Mod1Mask) && (s & ShiftMask)) {
                XCloseDisplay(dpy); 
                char *const rl[] = { "/usr/local/bin/sixwm", NULL }; 
                execvp(rl[0], rl);
            }
        }
    }
    XCloseDisplay(dpy); 
    return 0;
}
