/* serial-terminal — GTK3 serial terminal with visible control characters.
 * Font: Everson Mono (Unicode Control Pictures U+2400-U+243F), Unifont fallback.
 * Config: libconfig -> config.cfg  (saved on exit)
 * Linux backend: termios  |  Windows backend: WinAPI
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <libconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <errno.h>
  #include <glob.h>
  #include <poll.h>
  #include <termios.h>
  #include <unistd.h>
#endif

#define APP_NAME      "Serial Port Terminal"
#define GLADE_FILE    "window1.glade"
#define DEFAULT_CONFIG "config.cfg"
#define SYSTEM_GLADE  "/usr/share/serial-terminal/window1.glade"
#define SYSTEM_CONFIG "/usr/share/serial-terminal/config.cfg"

typedef enum { PARITY_NONE, PARITY_ODD, PARITY_EVEN, PARITY_MARK, PARITY_SPACE } Parity;
typedef enum { FLOW_NONE, FLOW_SOFT, FLOW_HARD } Flow;

typedef struct {
    GtkWidget *window, *port_combo, *baud_combo, *databits_combo, *parity_combo,
              *stopbits_combo, *flow_combo, *nlsend_combo, *connect_button,
              *rx_view, *tx_entry, *send_button, *clear_button,
              *show_control_check, *hex_check, *ts_check, *font_size_spin,
              *statusbar;
    GtkTextBuffer *buffer;
    GtkTextMark   *scroll_mark;
    GtkCssProvider *css;
    guint status_id;

    gint  fd;                 /* serial handle */
    GMutex serial_lock;
    GThread *read_thread;
    volatile gboolean running;    /* read thread keep-alive */
    volatile gboolean connected;

    gint   font_size;
    gchar *config_path;

    /* UTF-8 decoder state (survives chunk boundaries) */
    gint   u_need, u_got;
    guchar u_buf[8];
} App;

static App app;

/* ---------- small helpers ---------- */

static void status(const gchar *msg) {
    gtk_statusbar_push(GTK_STATUSBAR(app.statusbar), app.status_id, msg);
}

/* text of the active combo item (or typed text for editable combos); caller frees */
static gchar *combo_text(GtkWidget *combo) {
    return gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
}

/* integer value of the active item (baud, databits) */
static gint combo_value(GtkWidget *combo, gint def) {
    gchar *s = combo_text(combo);
    if (!s) return def;
    gint v = atoi(s);
    g_free(s);
    return v == 0 ? def : v;
}

/* select the item matching text (used when loading config) */
static void set_combo_by_text(GtkWidget *combo, const gchar *text) {
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter it; gint idx = 0;
    if (gtk_tree_model_get_iter_first(m, &it)) do {
        GValue val = G_VALUE_INIT;
        gtk_tree_model_get_value(m, &it, 0, &val);
        const gchar *row = g_value_get_string(&val);
        if (row && g_strcmp0(row, text) == 0) {
            g_value_unset(&val);
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx);
            return;
        }
        g_value_unset(&val);
        idx++;
    } while (gtk_tree_model_iter_next(m, &it));
}

static gchar *port_text(void) { return combo_text(app.port_combo); }

static Parity parity_from_str(const gchar *s) {
    if (g_strcmp0(s, "odd") == 0)   return PARITY_ODD;
    if (g_strcmp0(s, "even") == 0)  return PARITY_EVEN;
    if (g_strcmp0(s, "mark") == 0)  return PARITY_MARK;
    if (g_strcmp0(s, "space") == 0) return PARITY_SPACE;
    return PARITY_NONE;
}
static Flow flow_from_str(const gchar *s) {
    if (g_strcmp0(s, "soft") == 0) return FLOW_SOFT;
    if (g_strcmp0(s, "hard") == 0) return FLOW_HARD;
    return FLOW_NONE;
}

/* ---------- config (libconfig) ---------- */

static gchar *resolve_config_path(void) {
    const gchar *env = g_getenv("SERIALTERM_CONFIG");
    if (env && *env) return g_strdup(env);
    if (g_file_test(DEFAULT_CONFIG, G_FILE_TEST_EXISTS)) return g_strdup(DEFAULT_CONFIG);
    FILE *probe = fopen(DEFAULT_CONFIG, "a");          /* writable cwd? use it (dev runs) */
    if (probe) { fclose(probe); return g_strdup(DEFAULT_CONFIG); }
    gchar *dir = g_build_filename(g_get_user_config_dir(), "serial-terminal", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *p = g_build_filename(dir, DEFAULT_CONFIG, NULL);
    g_free(dir);
    if (!g_file_test(p, G_FILE_TEST_EXISTS) && g_file_test(SYSTEM_CONFIG, G_FILE_TEST_EXISTS)) {
        gchar *content = NULL; gsize len = 0;          /* seed from system default */
        if (g_file_get_contents(SYSTEM_CONFIG, &content, &len, NULL)) {
            g_file_set_contents(p, content, len, NULL);
            g_free(content);
        }
    }
    return p;
}

static void load_config(void) {
    config_t cfg;
    config_init(&cfg);
    if (config_read_file(&cfg, app.config_path) != CONFIG_TRUE) { config_destroy(&cfg); return; }
    const char *s; int i; int b;
    if (config_lookup_string(&cfg, "port.name", &s))
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(app.port_combo))), s);
    if (config_lookup_int(&cfg, "port.baud", &i)) {
        gchar tmp[16]; g_snprintf(tmp, sizeof tmp, "%d", i);
        set_combo_by_text(app.baud_combo, tmp);
    }
    if (config_lookup_int(&cfg, "port.databits", &i)) {
        gchar tmp[8]; g_snprintf(tmp, sizeof tmp, "%d", i);
        set_combo_by_text(app.databits_combo, tmp);
    }
    if (config_lookup_string(&cfg, "port.parity", &s)) set_combo_by_text(app.parity_combo, s);
    if (config_lookup_string(&cfg, "port.stopbits", &s)) set_combo_by_text(app.stopbits_combo, s);
    if (config_lookup_string(&cfg, "port.flow", &s)) set_combo_by_text(app.flow_combo, s);
    if (config_lookup_bool(&cfg, "display.show_control", &b))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.show_control_check), b);
    if (config_lookup_bool(&cfg, "display.hex", &b))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.hex_check), b);
    if (config_lookup_bool(&cfg, "display.timestamps", &b))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.ts_check), b);
    if (config_lookup_string(&cfg, "display.nlsend", &s)) set_combo_by_text(app.nlsend_combo, s);
    if (config_lookup_int(&cfg, "display.font_size", &i) && i > 0)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.font_size_spin), i);
    if (config_lookup_int(&cfg, "window.width", &i)) {
        int w = 0, h = 0;
        config_lookup_int(&cfg, "window.width", &w);
        config_lookup_int(&cfg, "window.height", &h);
        if (w > 0 && h > 0) gtk_window_resize(GTK_WINDOW(app.window), w, h);
    }
    config_destroy(&cfg);
}

static void save_config(void) {
    config_t cfg;
    config_init(&cfg);
    config_setting_t *root = config_root_setting(&cfg);

    config_setting_t *port = config_setting_add(root, "port", CONFIG_TYPE_GROUP);
    gchar *p = port_text();
    config_setting_set_string(config_setting_add(port, "name", CONFIG_TYPE_STRING), p ? p : "");
    g_free(p);
    config_setting_set_int(config_setting_add(port, "baud", CONFIG_TYPE_INT),
                           combo_value(app.baud_combo, 9600));
    config_setting_set_int(config_setting_add(port, "databits", CONFIG_TYPE_INT),
                           combo_value(app.databits_combo, 8));
    p = combo_text(app.parity_combo);
    config_setting_set_string(config_setting_add(port, "parity", CONFIG_TYPE_STRING), p ? p : "none");
    g_free(p);
    p = combo_text(app.stopbits_combo);
    config_setting_set_string(config_setting_add(port, "stopbits", CONFIG_TYPE_STRING), p ? p : "1");
    g_free(p);
    p = combo_text(app.flow_combo);
    config_setting_set_string(config_setting_add(port, "flow", CONFIG_TYPE_STRING), p ? p : "none");
    g_free(p);

    config_setting_t *disp = config_setting_add(root, "display", CONFIG_TYPE_GROUP);
    config_setting_set_bool(config_setting_add(disp, "show_control", CONFIG_TYPE_BOOL),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.show_control_check)));
    config_setting_set_bool(config_setting_add(disp, "hex", CONFIG_TYPE_BOOL),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.hex_check)));
    config_setting_set_bool(config_setting_add(disp, "timestamps", CONFIG_TYPE_BOOL),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.ts_check)));
    p = combo_text(app.nlsend_combo);
    config_setting_set_string(config_setting_add(disp, "nlsend", CONFIG_TYPE_STRING), p ? p : "crlf");
    g_free(p);
    config_setting_set_int(config_setting_add(disp, "font_size", CONFIG_TYPE_INT),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.font_size_spin)));

    config_setting_t *win = config_setting_add(root, "window", CONFIG_TYPE_GROUP);
    gint w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(app.window), &w, &h);
    config_setting_set_int(config_setting_add(win, "width", CONFIG_TYPE_INT), w);
    config_setting_set_int(config_setting_add(win, "height", CONFIG_TYPE_INT), h);

    if (config_write_file(&cfg, app.config_path) != CONFIG_TRUE)
        g_printerr("Could not write %s\n", app.config_path);
    config_destroy(&cfg);
}

/* ---------- serial backend ---------- */

#ifndef _WIN32
static speed_t baud_speed(gint baud) {
    switch (baud) {
        case 300: return B300;     case 600: return B600;
        case 1200: return B1200;   case 2400: return B2400;
        case 4800: return B4800;   case 9600: return B9600;
        case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200;
        case 230400: return B230400; case 460800: return B460800;
        case 921600: return B921600;
        default: return B9600;
    }
}

static gint serial_open(const gchar *port, gint baud, gint databits, Parity parity,
                        gint stopbits, Flow flow, gchar **err) {
    gint fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { if (err) *err = g_strdup_printf("Cannot open %s: %s", port, strerror(errno)); return -1; }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        if (err) *err = g_strdup_printf("tcgetattr: %s", strerror(errno));
        close(fd); return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    switch (databits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8;
    }
    if (parity == PARITY_EVEN)      tio.c_cflag |= PARENB;
    else if (parity == PARITY_ODD)  tio.c_cflag |= (PARENB | PARODD);
    else if (parity == PARITY_MARK) tio.c_cflag |= (PARENB | PARODD | CMSPAR);
    else if (parity == PARITY_SPACE)tio.c_cflag |= (PARENB | CMSPAR);
    if (stopbits == 2 || stopbits == 15) tio.c_cflag |= CSTOPB;   /* 1.5 -> CSTOPB approx */
    if (flow == FLOW_HARD) tio.c_cflag |= CRTSCTS;
    if (flow == FLOW_SOFT) tio.c_iflag |= (IXON | IXOFF);
    cfsetispeed(&tio, baud_speed(baud));
    cfsetospeed(&tio, baud_speed(baud));
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (err) *err = g_strdup_printf("tcsetattr: %s", strerror(errno));
        close(fd); return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}
static void serial_close(gint fd) { if (fd >= 0) close(fd); }
static gssize serial_write(gint fd, const gchar *buf, gsize len) {
    gsize off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (gsize)n;
    }
    return (gssize)off;
}

#else /* _WIN32 */

static gint serial_open(const gchar *port, gint baud, gint databits, Parity parity,
                        gint stopbits, Flow flow, gchar **err) {
    gchar full[64];
    g_snprintf(full, sizeof full, "\\\\.\\%s", port);   /* handles COM10+ */
    HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = g_strdup_printf("Cannot open %s (error %lu)", port, GetLastError());
        return -1;
    }
    DCB dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = (BYTE)databits;
    dcb.Parity = (parity == PARITY_ODD) ? ODDPARITY :
                 (parity == PARITY_EVEN) ? EVENPARITY :
                 (parity == PARITY_MARK) ? MARKPARITY :
                 (parity == PARITY_SPACE) ? SPACEPARITY : NOPARITY;
    dcb.StopBits = (stopbits == 1) ? ONESTOPBIT : TWOSTOPBITS;
    dcb.fParity = (dcb.Parity != NOPARITY);
    dcb.fOutxCtsFlow = (flow == FLOW_HARD);
    dcb.fRtsControl = (flow == FLOW_HARD) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE;
    dcb.fOutX = dcb.fInX = (flow == FLOW_SOFT);
    SetCommState(h, &dcb);
    COMMTIMEOUTS to = {0};                       /* never block forever in the read thread */
    to.ReadIntervalTimeout = 20;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 50;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return (gint)(intptr_t)h;
}
static void serial_close(gint fd) { if (fd >= 0) CloseHandle((HANDLE)(intptr_t)fd); }
static gssize serial_write(gint fd, const gchar *buf, gsize len) {
    DWORD written = 0;
    if (!WriteFile((HANDLE)(intptr_t)fd, buf, (DWORD)len, &written, NULL)) return -1;
    return (gssize)written;
}
#endif

/* ---------- receive path ---------- */

typedef struct { gchar *data; gsize len; } RxChunk;

static void insert_text(const gchar *text, gint len) {
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(app.buffer, &iter);
    gtk_text_buffer_insert(app.buffer, &iter, text, len);
    if (app.scroll_mark) {
        gtk_text_buffer_get_end_iter(app.buffer, &iter);
        gtk_text_buffer_move_mark(app.buffer, app.scroll_mark, &iter);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app.rx_view), app.scroll_mark,
                                     0.0, TRUE, 0.0, 1.0);
    }
}

/* insert the Control Picture for a control byte, e.g. \r -> U+240D "␍" */
static void insert_cp(guchar b) {
    gunichar cp = (b == 0x7F) ? 0x2421 : 0x2400 + b;   /* DEL -> ␡, C0 -> ␀..␟ */
    gchar tmp[8];
    gint n = g_unichar_to_utf8(cp, tmp);
    insert_text(tmp, n);
}

/* stateful byte->UTF-8 decoder + control mapping (called from main thread) */
static void decode_byte(guchar b) {
    if (app.u_need > 0) {                              /* middle of a multibyte seq */
        if ((b & 0xC0) != 0x80) { app.u_need = 0; app.u_got = 0; } /* invalid: drop */
        else {
            app.u_buf[app.u_got++] = b;
            if (app.u_got == app.u_need) {
                insert_text((gchar *)app.u_buf, app.u_need);
                app.u_need = 0; app.u_got = 0;
            }
        }
        return;
    }
    if (b < 0x80) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.show_control_check))) {
            switch (b) {
                case '\n': insert_text("\u240A\n", -1); break;   /* ␊ + real newline (wraps) */
                case '\r': insert_text("\u240D", -1);  break;   /* ␍ */
                case '\t': insert_text("\u2409\t", -1); break;  /* ␉ + real tab */
                default:
                    if (b < 0x20 || b == 0x7F) insert_cp(b);
                    else { gchar ch = (gchar)b; insert_text(&ch, 1); }
            }
        } else {
            gchar ch = (gchar)b; insert_text(&ch, 1);
        }
        return;
    }
    gint need = 0;                                      /* start of multibyte UTF-8 */
    if ((b & 0xE0) == 0xC0) need = 2;
    else if ((b & 0xF0) == 0xE0) need = 3;
    else if ((b & 0xF8) == 0xF0) need = 4;
    if (need == 0) { insert_text("\uFFFD", -1); return; }  /* invalid lead byte */
    app.u_need = need; app.u_got = 1; app.u_buf[0] = b;
}

static void append_rx(const gchar *data, gsize len) {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.hex_check))) {
        GString *s = g_string_sized_new(len * 3 + 16);
        for (gsize i = 0; i < len; i++) {
            if (i > 0 && i % 16 == 0) g_string_append_c(s, '\n');
            g_string_append_printf(s, "%02X ", (guchar)data[i]);
        }
        g_string_append_c(s, '\n');
        insert_text(s->str, (gint)s->len);
        g_string_free(s, TRUE);
        return;
    }
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.ts_check))) {
        GDateTime *dt = g_date_time_new_now_local();
        gchar *ts = g_date_time_format(dt, "[%H:%M:%S] ");
        insert_text(ts, -1);
        g_free(ts);
        g_date_time_unref(dt);
    }
    for (gsize i = 0; i < len; i++) decode_byte((guchar)data[i]);
}

static gboolean idle_rx(gpointer user) {
    RxChunk *c = user;
    append_rx(c->data, c->len);
    g_free(c->data);
    g_free(c);
    return G_SOURCE_REMOVE;               /* one-shot, not G_SOURCE_CONTINUE */
}

static gboolean idle_device_lost(gpointer user) {
    (void)user;
    status("Connection lost");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.connect_button), FALSE);
    return G_SOURCE_REMOVE;
}

static gpointer read_thread_main(gpointer user) {
    (void)user;
    gchar buf[4096];
    while (app.running) {
        g_mutex_lock(&app.serial_lock);
        gint fd = app.fd;
        g_mutex_unlock(&app.serial_lock);
        if (fd < 0) break;
#ifdef _WIN32
        DWORD n = 0;
        if (ReadFile((HANDLE)(intptr_t)fd, buf, sizeof(buf), &n, NULL) && n > 0) {
            RxChunk *c = g_new(RxChunk, 1);
            c->data = g_new(gchar, n); memcpy(c->data, buf, n); c->len = n;
            g_idle_add(idle_rx, c);
        } else {
            Sleep(50);                     /* bounded by COMMTIMEOUTS + this sleep */
        }
#else
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        gint pr = poll(&pfd, 1, 200);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                RxChunk *c = g_new(RxChunk, 1);
                c->data = g_new(gchar, n); memcpy(c->data, buf, n); c->len = (gsize)n;
                g_idle_add(idle_rx, c);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                if (app.running) g_idle_add(idle_device_lost, NULL);  /* unplugged */
                break;
            }
        }
#endif
    }
    return NULL;
}

/* ---------- actions ---------- */

static void do_send(void) {
    if (!app.connected || app.fd < 0) { status("Not connected"); return; }
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app.tx_entry));
    gsize len = strlen(text);
    if (len == 0) return;
    GString *out = g_string_new(text);
    gchar *nl = combo_text(app.nlsend_combo);
    if (g_strcmp0(nl, "cr") == 0) g_string_append_c(out, '\r');
    else if (g_strcmp0(nl, "lf") == 0) g_string_append_c(out, '\n');
    else if (g_strcmp0(nl, "crlf") == 0) g_string_append(out, "\r\n");
    g_free(nl);

    g_mutex_lock(&app.serial_lock);
    gint fd = app.fd;
    gssize n = (fd >= 0) ? serial_write(fd, out->str, out->len) : -1;
    g_mutex_unlock(&app.serial_lock);

    if (n < 0) {
        status("Write failed");
        g_string_free(out, TRUE);
        return;
    }

    append_rx(out->str, out->len);   /* echo TX exactly like RX */
    g_string_free(out, TRUE);
    gtk_entry_set_text(GTK_ENTRY(app.tx_entry), "");
}

static void on_connect_toggled(GtkToggleButton *btn, gpointer user) {
    (void)user;
    if (gtk_toggle_button_get_active(btn)) {
        gchar *port = port_text();
        if (!port || !*port) {
            g_free(port);
            gtk_toggle_button_set_active(btn, FALSE);
            status("No port selected");
            return;
        }
        gint baud   = combo_value(app.baud_combo, 9600);
        gint dbits  = combo_value(app.databits_combo, 8);
        gchar *pars = combo_text(app.parity_combo);
        gchar *sts  = combo_text(app.stopbits_combo);
        gchar *fls  = combo_text(app.flow_combo);
        Parity parity = parity_from_str(pars);
        gint sbits = (g_strcmp0(sts, "1.5") == 0) ? 15 : combo_value(app.stopbits_combo, 1);
        Flow flow = flow_from_str(fls);
        g_free(pars); g_free(sts); g_free(fls);

        gchar *err = NULL;
        gint fd = serial_open(port, baud, dbits, parity, sbits, flow, &err);
        if (fd < 0) {
            gtk_toggle_button_set_active(btn, FALSE);
            status(err ? err : "Open failed");
            g_free(err);
            g_free(port);
            return;
        }
        g_mutex_lock(&app.serial_lock);
        app.fd = fd;
        g_mutex_unlock(&app.serial_lock);
        app.connected = TRUE;
        app.running = TRUE;
        app.read_thread = g_thread_new("serial-read", read_thread_main, NULL);
        gchar *msg = g_strdup_printf("Connected: %s @ %d", port, baud);
        status(msg);
        g_free(msg);
        g_free(port);
        gtk_button_set_label(GTK_BUTTON(btn), "Disconnect");
    } else {
        if (app.connected) {
            app.running = FALSE;                        /* 1. stop thread */
            if (app.read_thread) { g_thread_join(app.read_thread); app.read_thread = NULL; }
            g_mutex_lock(&app.serial_lock);             /* 2. then close fd */
            gint fd = app.fd; app.fd = -1;
            g_mutex_unlock(&app.serial_lock);
            if (fd >= 0) serial_close(fd);
            app.connected = FALSE;
        }
        gtk_button_set_label(GTK_BUTTON(btn), "Connect");
        status("Disconnected");
    }
}

static void on_send_clicked(GtkButton *b, gpointer user) { (void)b; (void)user; do_send(); }

static void on_clear_clicked(GtkButton *b, gpointer user) {
    (void)b; (void)user;
    gtk_text_buffer_set_text(app.buffer, "", -1);
    if (app.scroll_mark) {
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(app.buffer, &it);
        gtk_text_buffer_move_mark(app.buffer, app.scroll_mark, &it);
    }
}

static void on_font_changed(GtkSpinButton *spin, gpointer user) {
    (void)user;
    app.font_size = gtk_spin_button_get_value_as_int(spin);
    gchar *css = g_strdup_printf(
        "#rx_view { font-family: \"Everson Mono\", \"Unifont\", monospace; font-size: %dpt; }",
        app.font_size);
    gtk_css_provider_load_from_data(app.css, css, -1, NULL);
    g_free(css);
}

static gboolean on_delete(GtkWidget *w, GdkEventAny *e, gpointer user) {
    (void)w; (void)e; (void)user;
    return FALSE;                          /* allow destroy */
}

static void on_destroy(GtkWidget *w, gpointer user) {
    (void)w; (void)user;
    if (app.connected) {                   /* same safe order: stop, join, close */
        app.running = FALSE;
        if (app.read_thread) { g_thread_join(app.read_thread); app.read_thread = NULL; }
        g_mutex_lock(&app.serial_lock);
        gint fd = app.fd; app.fd = -1;
        g_mutex_unlock(&app.serial_lock);
        if (fd >= 0) serial_close(fd);
        app.connected = FALSE;
    }
    save_config();                         /* persist last port + settings */
    gtk_main_quit();
}

/* ---------- UI setup ---------- */

static gchar *find_glade_file(void) {
    if (g_file_test(GLADE_FILE, G_FILE_TEST_EXISTS)) return g_strdup(GLADE_FILE);
    if (g_file_test(SYSTEM_GLADE, G_FILE_TEST_EXISTS)) return g_strdup(SYSTEM_GLADE);
#ifdef _WIN32
    wchar_t wbuf[MAX_PATH];
    GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    gchar *exe = g_utf16_to_utf8((const gunichar2 *)wbuf, -1, NULL, NULL, NULL);
    gchar *dir = g_path_get_dirname(exe);
    g_free(exe);
#else
    gchar *target = g_file_read_link("/proc/self/exe", NULL);
    gchar *dir = target ? g_path_get_dirname(target) : g_strdup(".");
    g_free(target);
#endif
    gchar *p = g_build_filename(dir, GLADE_FILE, NULL);
    g_free(dir);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
    g_printerr("Cannot find %s\n", GLADE_FILE);
    return NULL;
}

static void populate_ports(void) {
    GtkComboBoxText *cb = GTK_COMBO_BOX_TEXT(app.port_combo);
#ifdef _WIN32
    for (gint i = 1; i <= 32; i++) {
        gchar s[8]; g_snprintf(s, sizeof s, "COM%d", i);
        gtk_combo_box_text_append(cb, NULL, s);
    }
#else
    const gchar *patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*",
                               "/dev/ttyAMA*", "/dev/rfcomm*", "/dev/ttyTHS*", NULL};
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (gint i = 0; patterns[i]; i++) {
        glob_t g;
        if (glob(patterns[i], 0, NULL, &g) == 0) {
            for (gsize j = 0; j < g.gl_pathc; j++)
                if (!g_hash_table_contains(seen, g.gl_pathv[j])) {
                    gtk_combo_box_text_append(cb, NULL, g.gl_pathv[j]);
                    g_hash_table_add(seen, g_strdup(g.gl_pathv[j]));
                }
            globfree(&g);
        }
    }
    g_hash_table_destroy(seen);
#endif
}

static void add_items(GtkComboBoxText *cb, const gchar *const *items) {
    for (gint i = 0; items[i]; i++) gtk_combo_box_text_append(cb, NULL, items[i]);
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    app.fd = -1;
    g_mutex_init(&app.serial_lock);

    gchar *glade = find_glade_file();
    if (!glade) return 1;
    GtkBuilder *builder = gtk_builder_new_from_file(glade);
    g_free(glade);

    app.window           = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    app.port_combo       = GTK_WIDGET(gtk_builder_get_object(builder, "port_combo"));
    app.baud_combo       = GTK_WIDGET(gtk_builder_get_object(builder, "baud_combo"));
    app.databits_combo   = GTK_WIDGET(gtk_builder_get_object(builder, "databits_combo"));
    app.parity_combo     = GTK_WIDGET(gtk_builder_get_object(builder, "parity_combo"));
    app.stopbits_combo   = GTK_WIDGET(gtk_builder_get_object(builder, "stopbits_combo"));
    app.flow_combo       = GTK_WIDGET(gtk_builder_get_object(builder, "flow_combo"));
    app.nlsend_combo     = GTK_WIDGET(gtk_builder_get_object(builder, "nlsend_combo"));
    app.connect_button   = GTK_WIDGET(gtk_builder_get_object(builder, "connect_button"));
    app.rx_view          = GTK_WIDGET(gtk_builder_get_object(builder, "rx_view"));
    app.tx_entry         = GTK_WIDGET(gtk_builder_get_object(builder, "tx_entry"));
    app.send_button      = GTK_WIDGET(gtk_builder_get_object(builder, "send_button"));
    app.clear_button     = GTK_WIDGET(gtk_builder_get_object(builder, "clear_button"));
    app.show_control_check = GTK_WIDGET(gtk_builder_get_object(builder, "show_control_check"));
    app.hex_check        = GTK_WIDGET(gtk_builder_get_object(builder, "hex_check"));
    app.ts_check         = GTK_WIDGET(gtk_builder_get_object(builder, "ts_check"));
    app.font_size_spin   = GTK_WIDGET(gtk_builder_get_object(builder, "font_size_spin"));
    app.statusbar        = GTK_WIDGET(gtk_builder_get_object(builder, "statusbar"));
    g_object_unref(builder);

    app.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.rx_view));
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(app.buffer, &start);
    app.scroll_mark = gtk_text_buffer_create_mark(app.buffer, "scroll_mark", &start, FALSE);

    app.css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(app.css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    app.status_id = gtk_statusbar_get_context_id(GTK_STATUSBAR(app.statusbar), "main");

    populate_ports();
    static const gchar *bauds[] = {"300","600","1200","2400","4800","9600","19200",
                                   "38400","57600","115200","230400","460800","921600",NULL};
    static const gchar *dbits[] = {"5","6","7","8",NULL};
    static const gchar *pars[]  = {"none","odd","even","mark","space",NULL};
    static const gchar *stops[] = {"1","1.5","2",NULL};
    static const gchar *flows[] = {"none","soft","hard",NULL};
    static const gchar *nls[]   = {"none","cr","lf","crlf",NULL};
    add_items(GTK_COMBO_BOX_TEXT(app.baud_combo), bauds);
    add_items(GTK_COMBO_BOX_TEXT(app.databits_combo), dbits);
    add_items(GTK_COMBO_BOX_TEXT(app.parity_combo), pars);
    add_items(GTK_COMBO_BOX_TEXT(app.stopbits_combo), stops);
    add_items(GTK_COMBO_BOX_TEXT(app.flow_combo), flows);
    add_items(GTK_COMBO_BOX_TEXT(app.nlsend_combo), nls);
    set_combo_by_text(app.baud_combo, "115200");
    set_combo_by_text(app.databits_combo, "8");
    set_combo_by_text(app.parity_combo, "none");
    set_combo_by_text(app.stopbits_combo, "1");
    set_combo_by_text(app.flow_combo, "none");
    set_combo_by_text(app.nlsend_combo, "crlf");

    app.config_path = resolve_config_path();
    load_config();
    on_font_changed(GTK_SPIN_BUTTON(app.font_size_spin), NULL);   /* apply font */

    g_signal_connect(app.connect_button, "toggled", G_CALLBACK(on_connect_toggled), NULL);
    g_signal_connect(app.send_button,    "clicked", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(app.tx_entry,       "activate", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(app.clear_button,   "clicked", G_CALLBACK(on_clear_clicked), NULL);
    g_signal_connect(app.font_size_spin, "value-changed", G_CALLBACK(on_font_changed), NULL);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_destroy), NULL);

    status("Ready");
    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
