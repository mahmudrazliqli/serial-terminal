/*
 * Serial Terminal — a simple but capable serial port terminal.
 *
 * GTK3 UI loaded from window1.glade, serial I/O via POSIX termios.
 * No external serial library required.
 *
 * Settings (last port, baud, parity, display options, ...) are saved to
 *   $XDG_CONFIG_HOME/serial-terminal/config.cfg   (usually ~/.config/...)
 * on connect and on exit, and restored at startup.
 *
 * Control characters (\n, \r, NUL, ...) can be shown as visible symbols
 * (U+2400 Control Pictures). The receive area uses a font stack that
 * includes glyphs for them: Cascadia Mono, Everson Mono, then monospace.
 *
 * Build:  make
 * Run:    ./serial-terminal
 *         (window1.glade is looked up in the current directory first,
 *          then in the compiled-in data dir (for installed copies),
 *          then next to the executable)
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define UI_FILE "window1.glade"
#define RECV_CHUNK 4096

typedef struct {
    /* widgets resolved from the glade file */
    GtkWidget       *window;
    GtkComboBoxText *combo_port;      /* editable */
    GtkComboBoxText *combo_baud;
    GtkComboBoxText *combo_databits;
    GtkComboBoxText *combo_parity;
    GtkComboBoxText *combo_stopbits;
    GtkComboBoxText *combo_flow;
    GtkComboBoxText *combo_lineend;
    GtkTextView     *text_receive;
    GtkEntry        *entry_send;
    GtkButton       *btn_connect;
    GtkButton       *btn_send;
    GtkStatusbar    *statusbar;
    GtkCheckButton  *chk_hex_recv;
    GtkCheckButton  *chk_hex_send;
    GtkCheckButton  *chk_ts;
    GtkCheckButton  *chk_autoscroll;
    GtkCheckButton  *chk_ctrl;

    /* serial state */
    int         fd;          /* open fd, -1 when closed      */
    GIOChannel *channel;
    guint       in_watch;    /* G_IO_IN watch on the fd      */
    guint       out_watch;   /* G_IO_OUT watch (tx flushing) */
    GString    *tx_buf;      /* pending transmit data        */
    gboolean    connected;
    int         hex_col;     /* bytes on the current hex line */

    guint status_ctx;
} App;

/* ------------------------------------------------------------------ */
/* signal handlers declared up front                                   */
/* ------------------------------------------------------------------ */
static void set_connected_false(App *a, const char *msg);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void status_msg(App *a, const char *msg)
{
    gtk_statusbar_remove_all(a->statusbar, a->status_ctx);
    gtk_statusbar_push(a->statusbar, a->status_ctx, msg);
}

static void save_config(App *a);
static void load_config(App *a);

static gint cmp_cstr(gconstpointer ap, gconstpointer bp)
{
    const char *a = *(const char *const *)ap;
    const char *b = *(const char *const *)bp;
    return strcmp(a, b);
}

/* ------------------------------------------------------------------ */
/* port discovery                                                      */
/* ------------------------------------------------------------------ */

static void scan_ports(App *a)
{
    static const char *patterns[] = {
        "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*",
        "/dev/ttyAMA*", "/dev/ttyTHS*", "/dev/ttyXRUSB*", "/dev/ttyO*",
    };
    gchar *current = gtk_combo_box_text_get_active_text(a->combo_port);
    GPtrArray *ports;
    size_t i;

    gtk_combo_box_text_remove_all(a->combo_port);
    ports = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < G_N_ELEMENTS(patterns); i++) {
        glob_t g;
        size_t k;

        memset(&g, 0, sizeof g);
        if (glob(patterns[i], 0, NULL, &g) == 0) {
            for (k = 0; k < g.gl_pathc; k++)
                g_ptr_array_add(ports, g_strdup(g.gl_pathv[k]));
        }
        globfree(&g);
    }

    g_ptr_array_sort(ports, cmp_cstr);

    {
        gint match = -1;
        const char *prev = NULL;
        gint idx = -1;

        for (i = 0; i < ports->len; i++) {
            const char *p = g_ptr_array_index(ports, i);
            if (prev && strcmp(prev, p) == 0)
                continue;               /* dedupe */
            prev = p;
            if (match < 0 && current && strcmp(p, current) == 0)
                match = idx + 1;        /* remember where our port landed */
            gtk_combo_box_text_append_text(a->combo_port, p);
            idx++;
        }

        if (current) {
            if (match >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_port), match);
            } else {
                /* typed a path that isn't among the scanned ones — keep it */
                GtkWidget *entry = gtk_bin_get_child(GTK_BIN(a->combo_port));
                gtk_entry_set_text(GTK_ENTRY(entry), current);
            }
            g_free(current);
        }
    }

    g_ptr_array_free(ports, TRUE);
}

/* ------------------------------------------------------------------ */
/* serial open / close                                                 */
/* ------------------------------------------------------------------ */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
#ifdef B1500000
    case 1500000: return B1500000;
#endif
    default:     return B9600;
    }
}

static gboolean on_serial_in(GIOChannel *ch, GIOCondition cond, gpointer data);
static gboolean on_serial_out(GIOChannel *ch, GIOCondition cond, gpointer data);

static gboolean open_serial(App *a, const char *port, int baud,
                            int databits, int parity, int stopbits,
                            int flow, gchar **err)
{
    struct termios tio;
    int fd;

    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        *err = g_strdup_printf("Can't open %s: %s", port, strerror(errno));
        return FALSE;
    }

    if (tcgetattr(fd, &tio) != 0) {
        *err = g_strdup_printf("tcgetattr failed on %s: %s", port, strerror(errno));
        close(fd);
        return FALSE;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;              /* ignore modem lines, enable rx */

    tio.c_cflag &= ~CSIZE;                      /* data bits */
    switch (databits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }

    tio.c_cflag &= ~(PARENB | PARODD);          /* parity */
    tio.c_iflag &= ~(INPCK | ISTRIP);
    switch (parity) {
    case 1:  tio.c_cflag |= PARENB; break;                    /* even */
    case 2:  tio.c_cflag |= PARENB | PARODD; break;           /* odd  */
#ifdef CMSPAR
    case 3:  tio.c_cflag |= PARENB | CMSPAR | PARODD; break;  /* mark  */
    case 4:  tio.c_cflag |= PARENB | CMSPAR; break;           /* space */
#endif
    default: break;                                           /* none  */
    }

    tio.c_cflag &= ~CSTOPB;                     /* stop bits */
    if (stopbits == 2)
        tio.c_cflag |= CSTOPB;

    tio.c_cflag &= ~CRTSCTS;                    /* flow control */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    if (flow == 1)
        tio.c_cflag |= CRTSCTS;                 /* hardware */
    else if (flow == 2)
        tio.c_iflag |= IXON | IXOFF;            /* software */

    {
        speed_t sp = baud_to_speed(baud);
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        *err = g_strdup_printf("tcsetattr failed on %s: %s", port, strerror(errno));
        close(fd);
        return FALSE;
    }
    tcflush(fd, TCIOFLUSH);

    a->fd = fd;
    a->channel = g_io_channel_unix_new(fd);
    a->in_watch = g_io_add_watch(a->channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                 on_serial_in, a);
    return TRUE;
}

static void close_serial(App *a)
{
    if (a->in_watch)  { g_source_remove(a->in_watch);  a->in_watch = 0; }
    if (a->out_watch) { g_source_remove(a->out_watch); a->out_watch = 0; }
    if (a->channel)   { g_io_channel_shutdown(a->channel, TRUE, NULL);
                        g_io_channel_unref(a->channel);  a->channel = NULL; }
    if (a->fd >= 0)   { close(a->fd); a->fd = -1; }
    g_string_truncate(a->tx_buf, 0);
    a->hex_col = 0;
}

/* ------------------------------------------------------------------ */
/* receiving                                                           */
/* ------------------------------------------------------------------ */

/* Encode U+2400 + b (SYMBOL FOR NULL .. SYMBOL FOR SUBSTITUTE) as UTF-8. */
static void append_ctrl_symbol(GString *out, unsigned char b)
{
    guint cp = 0x2400 + b;
    g_string_append_c(out, (char)(0xC0 | (cp >> 6)));
    g_string_append_c(out, (char)(0x80 | (cp & 0x3F)));
}

/* Append received bytes, optionally turning control characters into the
 * visible U+2400 symbols so \n / \r / NUL / ... can be seen. A LF also
 * produces a real line break so the output stays readable. */
static void append_text_mode(App *a, GString *out,
                             const unsigned char *data, size_t len)
{
    gboolean show = gtk_toggle_button_get_active(
                        GTK_TOGGLE_BUTTON(a->chk_ctrl));
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char b = data[i];
        if (show && b < 0x20) {
            switch (b) {
            case '\n':
                g_string_append(out, "\u240A\n");   /* ␊ + line break */
                break;
            case '\r':
                g_string_append(out, "\u240D");      /* ␍ */
                break;
            default:
                append_ctrl_symbol(out, b);
                break;
            }
        } else if (show && b == 0x7F) {
            g_string_append(out, "\u2421");          /* ␡ DEL */
        } else {
            g_string_append_c(out, (char)b);
        }
    }
}

static void append_text(App *a, const char *text, gssize len)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(a->text_receive);
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, (gint)len);

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_autoscroll))) {
        GtkTextIter e;
        gtk_text_buffer_get_end_iter(buf, &e);
        gtk_text_view_scroll_to_iter(a->text_receive, &e, 0.0, TRUE, 0.0, 1.0);
    }
}

static void append_recv(App *a, const unsigned char *data, size_t len)
{
    gboolean hex = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_hex_recv));
    gboolean ts  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_ts));
    GString *out = g_string_new(NULL);
    size_t i;

    if (ts) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        g_string_append_printf(out, "[%02d:%02d:%02d] ",tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    if (hex) {
        for (i = 0; i < len; i++) {
            unsigned char b = data[i];
            if (b == '\n') {
                if (out->len && out->str[out->len - 1] == ' ')
                    g_string_truncate(out, out->len - 1);
                g_string_append_c(out, '\n');
                a->hex_col = 0;
            } else {
                g_string_append_printf(out, "%02X ", b);
                if (++a->hex_col >= 16) {
                    if (out->len && out->str[out->len - 1] == ' ')
                        g_string_truncate(out, out->len - 1);
                    g_string_append_c(out, '\n');
                    a->hex_col = 0;
                }
            }
        }
        if (out->len && out->str[out->len - 1] == ' ')
            g_string_truncate(out, out->len - 1);
    } else {
        append_text_mode(a, out, data, len);
    }

    append_text(a, out->str, out->len);
    g_string_free(out, TRUE);
}

static gboolean on_serial_in(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    App *a = data;
    unsigned char buf[RECV_CHUNK];
    (void)ch;

    ssize_t n;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        close_serial(a);
        set_connected_false(a, "Connection lost");
        return G_SOURCE_REMOVE;
    }

    n = read(a->fd, buf, sizeof buf);
    if (n > 0) {
        append_recv(a, buf, (size_t)n);
        return G_SOURCE_CONTINUE;
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close_serial(a);
        set_connected_false(a, "Connection closed");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* transmitting                                                        */
/* ------------------------------------------------------------------ */

static void set_connected_false(App *a, const char *msg)
{
    if (a->connected) {
        a->connected = FALSE;
        gtk_button_set_label(a->btn_connect, "Connect");
        gtk_widget_set_sensitive(GTK_WIDGET(a->entry_send), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(a->btn_send), FALSE);
    }
    status_msg(a, msg);
}

static void tx_flush(App *a);

static gboolean on_serial_out(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    App *a = data;

    (void)ch;
    if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        close_serial(a);
        set_connected_false(a, "Write failed");
        return G_SOURCE_REMOVE;
    }
    tx_flush(a);
    return G_SOURCE_CONTINUE;
}

static void tx_flush(App *a)
{
    if (a->fd < 0)
        return;

    while (a->tx_buf->len > 0) {
        ssize_t w = write(a->fd, a->tx_buf->str, a->tx_buf->len);
        if (w > 0) {
            g_string_erase(a->tx_buf, 0, (gsize)w);
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!a->out_watch)
                a->out_watch = g_io_add_watch(a->channel, G_IO_OUT,
                                              on_serial_out, a);
            return;
        } else {
            close_serial(a);
            set_connected_false(a, "Write failed");
            return;
        }
    }

    if (a->out_watch) {
        g_source_remove(a->out_watch);
        a->out_watch = 0;
    }
}

static void tx_enqueue(App *a, const char *data, gsize len)
{
    if (a->fd < 0)
        return;
    g_string_append_len(a->tx_buf, data, len);
    tx_flush(a);
}

/* ------------------------------------------------------------------ */
/* settings persistence (config.cfg)                                   */
/* ------------------------------------------------------------------ */

static const int bauds[] = { 1200, 2400, 4800, 9600, 19200, 38400,
                             57600, 115200, 230400, 460800, 921600 };

static gchar *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "serial-terminal",
                            "config.cfg", NULL);
}

static int find_int(const int *arr, size_t n, int val)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (arr[i] == val)
            return (int)i;
    return -1;
}

static void set_combo_index(GtkComboBoxText *cb, int idx)
{
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(cb));
    gint n = m ? gtk_tree_model_iter_n_children(m, NULL) : 0;
    if (idx >= 0 && idx < n)
        gtk_combo_box_set_active(GTK_COMBO_BOX(cb), idx);
}

static gboolean key_bool(GKeyFile *kf, const char *key, gboolean dflt)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(kf, "terminal", key, &err);
    if (err) {
        g_clear_error(&err);
        return dflt;
    }
    return v;
}

static void save_config(App *a)
{
    gchar *path = config_path();
    gchar *dir = g_path_get_dirname(path);
    gchar *port, *baud, *data, *stop, *lineend;
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    port    = gtk_combo_box_text_get_active_text(a->combo_port);
    baud    = gtk_combo_box_text_get_active_text(a->combo_baud);
    data    = gtk_combo_box_text_get_active_text(a->combo_databits);
    stop    = gtk_combo_box_text_get_active_text(a->combo_stopbits);
    lineend = gtk_combo_box_text_get_active_text(a->combo_lineend);

    g_key_file_set_string(kf, "terminal", "port"            , port ? port : "");
    g_key_file_set_integer(kf, "terminal", "baud"           , baud ? atoi(baud) : 9600);
    g_key_file_set_integer(kf, "terminal", "databits"       , data ? atoi(data) : 8);
    g_key_file_set_integer(kf, "terminal", "parity"         ,gtk_combo_box_get_active(GTK_COMBO_BOX(a->combo_parity)));
    g_key_file_set_integer(kf, "terminal", "stopbits"       , stop ? atoi(stop) : 1);
    g_key_file_set_integer(kf, "terminal", "flow"           ,gtk_combo_box_get_active(GTK_COMBO_BOX(a->combo_flow)));
    g_key_file_set_integer(kf, "terminal", "lineend"        ,gtk_combo_box_get_active(GTK_COMBO_BOX(a->combo_lineend)));
    g_key_file_set_boolean(kf, "terminal", "hex_recv"       ,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_hex_recv)));
    g_key_file_set_boolean(kf, "terminal", "hex_send"       ,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_hex_send)));
    g_key_file_set_boolean(kf, "terminal", "timestamp"      ,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_ts)));
    g_key_file_set_boolean(kf, "terminal", "autoscroll"     ,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_autoscroll)));
    g_key_file_set_boolean(kf, "terminal", "show_controls"  ,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_ctrl)));

    g_free(port);
    g_free(baud);
    g_free(data);
    g_free(stop);
    g_free(lineend);

    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("serial-terminal: could not save %s: %s\n", path,
                   err ? err->message : "unknown error");
        g_clear_error(&err);
    }

    g_key_file_free(kf);
    g_free(path);
}

static void load_config(App *a)
{
    gchar *path = config_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    gchar *port;
    int v;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error(&err);          /* no config yet — first run */
        g_key_file_free(kf);
        g_free(path);
        return;
    }

    port = g_key_file_get_string(kf, "terminal", "port", NULL);
    if (port && *port) {
        GtkWidget *entry = gtk_bin_get_child(GTK_BIN(a->combo_port));
        gtk_entry_set_text(GTK_ENTRY(entry), port);
    }
    g_free(port);

    v = g_key_file_get_integer(kf, "terminal", "baud", NULL);
    set_combo_index(a->combo_baud, find_int(bauds, G_N_ELEMENTS(bauds), v));

    v = g_key_file_get_integer(kf, "terminal", "databits", NULL);
    set_combo_index(a->combo_databits, v >= 5 && v <= 8 ? v - 5 : -1);

    v = g_key_file_get_integer(kf, "terminal", "parity", NULL);
    set_combo_index(a->combo_parity, v);

    v = g_key_file_get_integer(kf, "terminal", "stopbits", NULL);
    set_combo_index(a->combo_stopbits, v >= 1 && v <= 2 ? v - 1 : -1);

    v = g_key_file_get_integer(kf, "terminal", "flow", NULL);
    set_combo_index(a->combo_flow, v);

    v = g_key_file_get_integer(kf, "terminal", "lineend", NULL);
    set_combo_index(a->combo_lineend, v);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_hex_recv)     ,key_bool(kf, "hex_recv", FALSE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_hex_send)     ,key_bool(kf, "hex_send", FALSE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_ts)           ,key_bool(kf, "timestamp", FALSE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_autoscroll)   ,key_bool(kf, "autoscroll", TRUE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->chk_ctrl)         ,key_bool(kf, "show_controls", TRUE));

    g_key_file_free(kf);
    g_free(path);
}

/* ------------------------------------------------------------------ */
/* widget setup                                                        */
/* ------------------------------------------------------------------ */

static void populate_combos(App *a)
{
    static const char *data[]   = { "5", "6", "7", "8" };
    static const char *parity[] = { "None", "Even", "Odd", "Mark", "Space" };
    static const char *stop[]   = { "1", "2" };
    static const char *flow[]   = { "None", "Hardware (RTS/CTS)",
                                    "Software (XON/XOFF)" };
    static const char *le[]     = { "None", "CR", "LF", "CR+LF" };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(bauds); i++) {
        char s[16];
        snprintf(s, sizeof s, "%d", bauds[i]);
        gtk_combo_box_text_append_text(a->combo_baud, s);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_baud), 3); /* 9600 */

    for (i = 0; i < G_N_ELEMENTS(data); i++)
        gtk_combo_box_text_append_text(a->combo_databits, data[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_databits), 3); /* 8 */

    for (i = 0; i < G_N_ELEMENTS(parity); i++)
        gtk_combo_box_text_append_text(a->combo_parity, parity[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_parity), 0); /* none */

    for (i = 0; i < G_N_ELEMENTS(stop); i++)
        gtk_combo_box_text_append_text(a->combo_stopbits, stop[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_stopbits), 0); /* 1 */

    for (i = 0; i < G_N_ELEMENTS(flow); i++)
        gtk_combo_box_text_append_text(a->combo_flow, flow[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_flow), 0); /* none */

    for (i = 0; i < G_N_ELEMENTS(le); i++)
        gtk_combo_box_text_append_text(a->combo_lineend, le[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->combo_lineend), 2); /* LF */
}

/* Search order: current directory, the compiled-in data dir (used when
 * installed via the .deb / make install), then next to the executable. */
static char *find_ui_file(void)
{
    gchar *exe, *dir, *path;

    if (g_file_test(UI_FILE, G_FILE_TEST_EXISTS))
        return g_strdup(UI_FILE);

#ifdef DATA_DIR
    path = g_build_filename(DATA_DIR, UI_FILE, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return path;
    g_free(path);
#endif

    exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        dir = g_path_get_dirname(exe);
        path = g_build_filename(dir, UI_FILE, NULL);
        g_free(dir);
        g_free(exe);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* signal handlers                                                     */
/* ------------------------------------------------------------------ */

void on_btn_connect_clicked(GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    (void)btn;

    if (a->connected) {
        close_serial(a);
        set_connected_false(a, "Disconnected");
        return;
    }

    {
        gchar *port = gtk_combo_box_text_get_active_text(a->combo_port);
        gchar *baud_s = gtk_combo_box_text_get_active_text(a->combo_baud);
        gchar *data_s = gtk_combo_box_text_get_active_text(a->combo_databits);
        gchar *stop_s = gtk_combo_box_text_get_active_text(a->combo_stopbits);
        int baud   = baud_s ? atoi(baud_s) : 9600;
        int data   = data_s ? atoi(data_s) : 8;
        int parity = gtk_combo_box_get_active(GTK_COMBO_BOX(a->combo_parity));
        int stop   = stop_s ? atoi(stop_s) : 1;
        int flow   = gtk_combo_box_get_active(GTK_COMBO_BOX(a->combo_flow));
        gchar *err = NULL;

        if (!port || !*port) {
            status_msg(a, "No port selected — pick or type a device path");
            g_free(port);
            g_free(baud_s);
            g_free(data_s);
            g_free(stop_s);
            return;
        }

        if (open_serial(a, port, baud, data, parity, stop, flow, &err)) {
            gchar *msg = g_strdup_printf("Connected to %s @ %d baud, %d%s%d",
                                         port, baud, data,
                                         parity == 1 ? "E" :
                                         parity == 2 ? "O" :
                                         parity == 3 ? "M" :
                                         parity == 4 ? "S" : "N",
                                         stop);
            a->connected = TRUE;
            gtk_button_set_label(a->btn_connect, "Disconnect");
            gtk_widget_set_sensitive(GTK_WIDGET(a->entry_send), TRUE);
            gtk_widget_set_sensitive(GTK_WIDGET(a->btn_send), TRUE);
            status_msg(a, msg);
            g_free(msg);
            save_config(a);
        } else {
            gchar *msg = err ? err : g_strdup("Failed to open port");
            set_connected_false(a, msg);
            g_free(msg);
        }

        g_free(err);
        g_free(port);
        g_free(baud_s);
        g_free(data_s);
        g_free(stop_s);
    }
}

static void do_send(App *a)
{
    const gchar *text;
    gchar *lineend;
    gboolean hex;
    GString *out;
    const char *p;

    if (!a->connected || a->fd < 0)
        return;

    text = gtk_entry_get_text(a->entry_send);
    if (!text || !*text)
        return;

    lineend = gtk_combo_box_text_get_active_text(a->combo_lineend);
    hex = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->chk_hex_send));
    out = g_string_new(NULL);

    if (hex) {
        p = text;
        while (*p) {
            char *end;
            long v;

            while (*p == ' ')
                p++;
            if (!*p)
                break;

            v = strtol(p, &end, 16);
            if (end == p) {
                status_msg(a, "Invalid hex input — use e.g. \"48 65 6C 6C 6F\"");
                g_string_free(out, TRUE);
                g_free(lineend);
                return;
            }
            if (v < 0 || v > 255) {
                status_msg(a, "Hex bytes must be 00..FF");
                g_string_free(out, TRUE);
                g_free(lineend);
                return;
            }
            g_string_append_c(out, (char)v);
            p = end;
            while (*p == ' ')
                p++;
            if (*p == ',' || *p == ';' || *p == ':')
                p++;
        }
    } else {
        g_string_append(out, text);
    }

    if (lineend) {
        if (strcmp(lineend, "CR") == 0)
            g_string_append_c(out, '\r');
        else if (strcmp(lineend, "LF") == 0)
            g_string_append_c(out, '\n');
        else if (strcmp(lineend, "CR+LF") == 0)
            g_string_append(out, "\r\n");
        g_free(lineend);
    }


    tx_enqueue(a, out->str, out->len);
    append_text(a, out->str, out->len);

   
    g_string_free(out, TRUE);
}

void on_btn_send_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    do_send(user_data);


}

void on_entry_send_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    do_send(user_data);
}

void on_btn_clear_clicked(GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(a->text_receive);
    (void)btn;
    gtk_text_buffer_set_text(buf, "", -1);
}

void on_btn_refresh_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    scan_ports(user_data);
}

void on_window1_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    save_config(user_data);
    gtk_main_quit();
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    GtkBuilder *builder;
    App *a;
    GError *err = NULL;
    gchar *ui_file;
    GtkCssProvider *css;

    gtk_init(&argc, &argv);

    ui_file = find_ui_file();
    if (!ui_file) {
        g_printerr("Could not find %s (current dir or executable dir)\n", UI_FILE);
        return 1;
    }

    builder = gtk_builder_new();
    if (!gtk_builder_add_from_file(builder, ui_file, &err)) {
        g_printerr("Failed to load UI from %s: %s\n", ui_file,
                   err ? err->message : "unknown error");
        g_clear_error(&err);
        return 1;
    }
    g_free(ui_file);

    a = g_new0(App, 1);
    a->fd = -1;
    a->tx_buf = g_string_new(NULL);

    a->window        = GTK_WIDGET(gtk_builder_get_object(builder, "window1"));
    a->combo_port    = GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_port"));
    a->combo_baud    = GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_baud"));
    a->combo_databits= GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_databits"));
    a->combo_parity  = GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_parity"));
    a->combo_stopbits= GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_stopbits"));
    a->combo_flow    = GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_flow"));
    a->combo_lineend = GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "combo_lineend"));
    a->text_receive  = GTK_TEXT_VIEW(gtk_builder_get_object(builder, "text_receive"));
    a->entry_send    = GTK_ENTRY(gtk_builder_get_object(builder, "entry_send"));
    a->btn_connect   = GTK_BUTTON(gtk_builder_get_object(builder, "btn_connect"));
    a->btn_send      = GTK_BUTTON(gtk_builder_get_object(builder, "btn_send"));
    a->statusbar     = GTK_STATUSBAR(gtk_builder_get_object(builder, "statusbar"));
    a->chk_hex_recv  = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "chk_hex_recv"));
    a->chk_hex_send  = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "chk_hex_send"));
    a->chk_ts        = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "chk_ts"));
    a->chk_autoscroll= GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "chk_autoscroll"));
    a->chk_ctrl      = GTK_CHECK_BUTTON(gtk_builder_get_object(builder, "chk_ctrl"));
    a->status_ctx    = gtk_statusbar_get_context_id(a->statusbar, "main");

    gtk_builder_connect_signals(builder, a);
    g_object_unref(builder);

    populate_combos(a);
    scan_ports(a);
    load_config(a);

    /* monospace font for the receive area. Cascadia Mono / Everson Mono
     * carry the U+2400 control-picture glyphs (\n, \r, NUL, ...); DejaVu
     * Sans Mono and plain monospace are fallbacks. */
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "textview#text_receive { font-family: \"Cascadia Mono\", \"Everson Mono\", \"DejaVu Sans Mono\", monospace; font-size: 10pt; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    gtk_widget_show(a->window);
    gtk_widget_grab_focus(GTK_WIDGET(a->entry_send));

    {
        gchar *cfg = config_path();
        gchar *msg = g_strdup_printf("Disconnected — settings: %s", cfg);
        status_msg(a, msg);
        g_free(msg);
        g_free(cfg);
    }

    gtk_main();

    close_serial(a);
    g_string_free(a->tx_buf, TRUE);
    g_free(a);
    return 0;
}
