/*
To compile and run:
gcc $(pkg-config --cflags gtk4) -o main main.c $(pkg-config --libs gtk4) -lm && ./main
(Linux and macOS; Windows via mingw, see README)
*/

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#ifdef _WIN32
#  include <windows.h>
   typedef HANDLE serial_fd_t;
   typedef DWORD speed_t;
#  define SERIAL_INVALID INVALID_HANDLE_VALUE
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <glob.h>
#  include <termios.h>
#  include <unistd.h>
   typedef int serial_fd_t;
#  define SERIAL_INVALID (-1)
#endif

#define VOLTAGE_MIN   0
#define VOLTAGE_MAX   5.0
#define VOLTAGE_STEP  0.0001   /* 100 µV */

// DAC70501 registers
const uint8_t REG_SYNC = 0x02;
const uint8_t REG_CONFIG = 0x03;
const uint8_t REG_GAIN = 0x04;
const uint8_t REG_TRIGGER = 0x05;
const uint8_t REG_DAC = 0x08;

// DAC count
const uint8_t NUM_BANKS = 3;
const uint8_t DACS_PER_BANK = 4;
const uint8_t TOTAL_DACS = NUM_BANKS * DACS_PER_BANK;
const uint8_t DAC_ADDR[4] = {0x48, 0x49, 0x4A, 0x4B}; 

// Microcontroller will use this starting byte to determine how to read the subsequent bytes
// GUI tells the microcontroller what to do. Microcontroller does the actual I2C/SPI.
const uint8_t DAC_CMD_START = 0x00; // writing voltage value to a DAC
const uint8_t RST_CMD_START = 0x01; // pulling down Reset pin
const uint8_t SPI_CMD_START = 0x02; // writing SPI command

uint16_t voltageToCode(float v) {
  if (v <= 0.0f) return 0x0000;
  if (v >= VOLTAGE_MAX) return 0xFFFF;
  uint32_t code = (uint32_t)(v / VOLTAGE_MAX * 65536.0f + 0.5f);
  return (uint16_t)(code > 0xFFFF ? 0xFFFF : code);
}

// Keep a global or static descriptor variable for the ongoing session
static serial_fd_t active_serial_fd = SERIAL_INVALID;

// Connection status line shown under the serial row (set in activate())
static GtkLabel *status_label = NULL;

#ifdef _WIN32
static GThread *win_reader = NULL;       // background reader thread
static gint     win_reader_stop = 0;     // set to ask the reader to exit
static guint    conn_generation = 0;     // bumped on every disconnect (ignores stale events)
#else
static guint    serial_watch_id = 0;     // GLib source id of the POSIX read watch
#endif

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (status_label) gtk_label_set_text(status_label, msg);
    g_free(msg);
}

// serial connection dropdowns (shared, heap-allocated in activate())
typedef struct {
    GtkDropDown *com_dropdown;
    GtkDropDown *baud_dropdown;
} ConnectData;

// SPI settings dropdowns
typedef struct {
    GtkDropDown *speed_dropdown;
    GtkDropDown *bit_order_dropdown;
    GtkDropDown *mode_dropdown;
} SPISettingsData;

// per-channel Update button data
typedef struct {
    int         ch;
    GtkWidget  *entry;
    ConnectData *conn; // pointer to the shared ConnectData
} BtnCallbackData;

// data for the "All OFF / All ON / All UPDATE" buttons
typedef struct {
    GtkToggleButton *off_buttons[12];    // the 12 per-channel toggle buttons
    BtnCallbackData *off_data[12];       // the 12 BtnCallbackData pointers for the off buttons
                                         // (needed to match the handler when blocking/unblocking signals)
    GtkWidget       *entry_boxes[12];    // the 12 per-channel voltage entries
    GtkWidget       *channel_labels[12]; // the "Channel N:" labels (presets add a name in parentheses)
    ConnectData     *conn;               // shared serial dropdowns
} AllBtnData;

// Radio button state to choose which bytes to send
typedef struct {
	GtkWidget *radio_btn0;
	GtkWidget *radio_btn1;
	GtkWidget *radio_btn2;
    GtkWidget *byte_entries[3];
    ConnectData *conn;
    SPISettingsData *settings;
} RadioBtnSelect;

// voltage helper functions
static double snap_and_clamp(double value) {
    value = round(value / VOLTAGE_STEP) * VOLTAGE_STEP;
    if (value < VOLTAGE_MIN) value = VOLTAGE_MIN;
    if (value > VOLTAGE_MAX) value = VOLTAGE_MAX;
    return value;
}

static void apply_value(GtkEntry *entry, double value) {
    value = snap_and_clamp(value);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", value);
    gtk_editable_set_text(GTK_EDITABLE(entry), buf);
}

static void on_up_clicked(GtkButton *button, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    apply_value(entry, value + VOLTAGE_STEP);
}

static void on_down_clicked(GtkButton *button, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    apply_value(entry, value - VOLTAGE_STEP);
}

// Cross platform serial communication

#ifdef _WIN32

static DWORD baud_from_string(const char *s) {
    return (DWORD)atoi(s); // Windows accepts the raw integer directly
}

static serial_fd_t open_serial(const char *port, DWORD baud) {
    // Windows requires the "\\.\COMn" prefix for ports above COM9
    char win_port[64];
    if (strncmp(port, "\\\\.\\", 4) != 0)
        snprintf(win_port, sizeof(win_port), "\\\\.\\%s", port);
    else
        snprintf(win_port, sizeof(win_port), "%s", port);

    HANDLE hPort = CreateFileA(
        win_port,
        GENERIC_READ | GENERIC_WRITE,
        0,       // no sharing
        NULL,    // default security
        OPEN_EXISTING,
        0,       // non-overlapped I/O
        NULL);
    if (hPort == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hPort, &dcb)) { CloseHandle(hPort); return INVALID_HANDLE_VALUE; }

    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;

    if (!SetCommState(hPort, &dcb)) { CloseHandle(hPort); return INVALID_HANDLE_VALUE; }

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout         = MAXDWORD; //50
    timeouts.ReadTotalTimeoutConstant    = 0; //500
    timeouts.ReadTotalTimeoutMultiplier  = 0; //10
    timeouts.WriteTotalTimeoutConstant   = 500;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hPort, &timeouts);

    return hPort;
}

// Returns 0 on success, -1 on failure
static int serial_write(serial_fd_t fd, const char *buf, size_t len) {
    DWORD written = 0;
    if (!WriteFile(fd, buf, (DWORD)len, &written, NULL) || written != (DWORD)len) return -1;
    return 0;
}

static void serial_close(serial_fd_t fd) {
    CloseHandle(fd);
}

static int serial_is_invalid(serial_fd_t fd) {
    return fd == INVALID_HANDLE_VALUE;
}

#else /* POSIX */

static speed_t baud_from_string(const char *s) {
    int b = atoi(s);
    switch (b) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        #ifdef __APPLE__
		    case 460800: return 460800;
		    case 921600: return 921600;
		#else
		    case 460800: return B460800;
		    case 921600: return B921600;
		#endif
        default:     return B115200;
    }
}

static serial_fd_t open_serial(const char *port, speed_t baud) {
    // Open non-blocking so a stuck modem line can't hang us; cleared again below
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) goto fail;

    // Raw mode: the protocol is binary, so no CR/LF translation or XON/XOFF
    // (otherwise bytes like 0x0D, 0x11, 0x13 would be altered or swallowed)
    cfmakeraw(&tty);
    tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;
    if (cfsetospeed(&tty, baud) != 0 || cfsetispeed(&tty, baud) != 0) goto fail;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) goto fail;

    tcflush(fd, TCIOFLUSH);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;

fail:;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

// Returns 0 on success, -1 on failure
static int serial_write(serial_fd_t fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void serial_close(serial_fd_t fd) {
    close(fd);
}

static int serial_is_invalid(serial_fd_t fd) {
    return fd < 0;
}

#endif /* _WIN32 */

// Serial port discovery

typedef struct {
    char    *path;        // what open_serial() takes, e.g. "/dev/cu.usbmodem2101" or "COM7"
    gboolean is_feather;  // looks like the USB CDC device the Feather presents
} PortInfo;

static void port_info_free(gpointer data) {
    PortInfo *info = data;
    g_free(info->path);
    g_free(info);
}

static void add_port(GPtrArray *ports, const char *path, gboolean is_feather) {
    PortInfo *info = g_new0(PortInfo, 1);
    info->path = g_strdup(path);
    info->is_feather = is_feather;
    g_ptr_array_add(ports, info);
}

// Feather-looking ports first, then shorter names first so COM9 sorts before COM10
static gint compare_ports(gconstpointer a, gconstpointer b) {
    const PortInfo *pa = *(PortInfo * const *)a;
    const PortInfo *pb = *(PortInfo * const *)b;
    if (pa->is_feather != pb->is_feather) return pa->is_feather ? -1 : 1;
    size_t la = strlen(pa->path), lb = strlen(pb->path);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(pa->path, pb->path);
}

#ifdef _WIN32

// Lists every COM port the driver stack has registered (no COM1-COM8 limit).
// Native USB CDC devices like the Feather show up as \Device\USBSERnnn.
static GPtrArray *enumerate_ports(void) {
    GPtrArray *ports = g_ptr_array_new_with_free_func(port_info_free);
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0; ; i++) {
            char  name[256];
            char  data[64] = { 0 };
            DWORD name_len = sizeof(name);
            DWORD data_len = sizeof(data) - 1;
            DWORD type = 0;
            LONG  r = RegEnumValueA(key, i, name, &name_len, NULL, &type,
                                    (LPBYTE)data, &data_len);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || type != REG_SZ) continue;
            add_port(ports, data, strstr(name, "USBSER") != NULL);
        }
        RegCloseKey(key);
    }
    g_ptr_array_sort(ports, compare_ports);
    return ports;
}

#else /* POSIX */

#ifdef __linux__
// Reads the USB vendor id of a tty from sysfs; Adafruit (239a) and Espressif (303a)
// are what a Feather ESP32-S3 enumerates as.
static gboolean tty_is_feather(const char *path) {
    char *name = g_path_get_basename(path);
    static const char *const rel[] = { "device/../idVendor", "device/../../idVendor" };
    gboolean found = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(rel) && !found; i++) {
        char *file = g_strdup_printf("/sys/class/tty/%s/%s", name, rel[i]);
        char *contents = NULL;
        if (g_file_get_contents(file, &contents, NULL, NULL)) {
            unsigned long vid = strtoul(contents, NULL, 16);
            found = (vid == 0x239a || vid == 0x303a);
            g_free(contents);
        }
        g_free(file);
    }
    g_free(name);
    return found;
}
#elif defined(__APPLE__)
// The Feather's native USB shows up as /dev/cu.usbmodem*
static gboolean tty_is_feather(const char *path) {
    return strstr(path, "usbmodem") != NULL;
}
#else
static gboolean tty_is_feather(const char *path) {
    (void)path;
    return FALSE;
}
#endif

static GPtrArray *enumerate_ports(void) {
    GPtrArray *ports = g_ptr_array_new_with_free_func(port_info_free);
#ifdef __APPLE__
    // Only the callout (cu.*) nodes: tty.* blocks on open waiting for carrier
    static const char *const patterns[] = {
        "/dev/cu.usbmodem*", "/dev/cu.usbserial*",
        "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*", NULL
    };
#else
    static const char *const patterns[] = { "/dev/ttyACM*", "/dev/ttyUSB*", NULL };
#endif
    for (int i = 0; patterns[i]; i++) {
        glob_t gl = { 0 };
        if (glob(patterns[i], 0, NULL, &gl) == 0) {
            for (size_t j = 0; j < gl.gl_pathc; j++)
                add_port(ports, gl.gl_pathv[j], tty_is_feather(gl.gl_pathv[j]));
        }
        globfree(&gl);
    }
    g_ptr_array_sort(ports, compare_ports);
    return ports;
}

#endif /* port discovery */

// Reads the selected COM port and baud rate strings from the shared dropdowns.
// Returns FALSE (leaving *port_out/*baud_out untouched) if either dropdown has nothing selected.
static gboolean get_port_and_baud(ConnectData *conn, const char **port_out, const char **baud_out) {
    GtkStringObject *com_obj  = GTK_STRING_OBJECT(
        gtk_drop_down_get_selected_item(conn->com_dropdown));
    GtkStringObject *baud_obj = GTK_STRING_OBJECT(
        gtk_drop_down_get_selected_item(conn->baud_dropdown));

    if (!com_obj || !baud_obj) return FALSE;

    *port_out = gtk_string_object_get_string(com_obj);
    *baud_out = gtk_string_object_get_string(baud_obj);
    return TRUE;
}

// Tears down the active connection (reader first, then the port). Safe to call when not connected.
static void disconnect_serial(void) {
    if (serial_is_invalid(active_serial_fd)) return;
#ifdef _WIN32
    if (win_reader) {
        g_atomic_int_set(&win_reader_stop, 1);
        g_thread_join(win_reader);
        win_reader = NULL;
    }
    conn_generation++;
#else
    if (serial_watch_id) {
        g_source_remove(serial_watch_id);
        serial_watch_id = 0;
    }
#endif
    serial_close(active_serial_fd);
    active_serial_fd = SERIAL_INVALID;
}

// The device went away (unplugged, port closed by the OS, write failed)
static void connection_lost(const char *why) {
    disconnect_serial();
    set_status("Disconnected: %s", why);
    g_printerr("Serial connection lost: %s\n", why);
}

static gboolean require_connection(void) {
    if (!serial_is_invalid(active_serial_fd)) return TRUE;
    set_status("Not connected. Choose a port and press Connect.");
    g_printerr("Error: Cannot send. Port not connected!\n");
    return FALSE;
}

// Writes to the persistent connection; the one place every command goes through.
static gboolean send_bytes(const void *buf, size_t len) {
    if (!require_connection()) return FALSE;
    if (serial_write(active_serial_fd, (const char *)buf, len) != 0) {
        connection_lost("write failed (was the board unplugged?)");
        return FALSE;
    }
    return TRUE;
}

// Sends 6 bytes: start, bank, chip, register, high half of value, low half of value.
static gboolean send_voltage_command(uint8_t reg, int channel, uint16_t value) {
    uint8_t bank = channel / DACS_PER_BANK;
    uint8_t chip = channel % DACS_PER_BANK;
    uint8_t high = (uint8_t)(value >> 8);
    uint8_t low = (uint8_t)(value & 0xFF);
    
    uint8_t msg[6];
    msg[0] = DAC_CMD_START; // This byte matters for microcontroller firmware
    msg[1] = bank;
    msg[2] = DAC_ADDR[chip]; // get the corresponding DAC address
    msg[3] = reg;
    msg[4] = high;
    msg[5] = low;

    g_print("Sending 0x: %02X %02X %02X %02X %02X\n", msg[1],msg[2],msg[3],msg[4],msg[5]);
    return send_bytes(msg, sizeof(msg));
}

// Updates an off/on toggle button's label and CSS name to match its state.
static void set_off_button_visual(GtkWidget *btn, gboolean is_on) {
    gtk_button_set_label(GTK_BUTTON(btn), is_on ? "ON" : "OFF");
    gtk_widget_set_name(btn, is_on ? "btn-on" : "btn-off");
}

// Allocates a BtnCallbackData for either an Off or Update button.
static BtnCallbackData *make_btn_data(int ch, GtkWidget *entry, ConnectData *conn) {
    BtnCallbackData *data = g_new(BtnCallbackData, 1);
    data->ch    = ch;
    data->entry = entry;
    data->conn  = conn;
    return data;
}

// Struct to hold widgets that display received SPI data
typedef struct {
    GtkWidget *received_labels[3];
} IncomingDataWidgets;

// Global pointer to background listeners
static IncomingDataWidgets *global_rx_widgets = NULL;

// Which "Bytes Received" label gets the next byte (0-2). Reset on every SPI send so a
// reply always starts at Byte 0. Atomic because the Windows reader runs on its own thread.
static gint rx_byte_index = 0;

// Send button callback depends on which radio button is selected
static void on_send_clicked(GtkButton *button, gpointer user_data) {
    RadioBtnSelect *data = (RadioBtnSelect *)user_data;
    
    // Get SPI speed from dropdowns
    GtkStringObject *speed_str = GTK_STRING_OBJECT(
        gtk_drop_down_get_selected_item(data->settings->speed_dropdown));
    uint32_t speed = (uint32_t)strtoul(gtk_string_object_get_string(speed_str), NULL, 10);

    // Get SPI Bit Order from dropdowns
    GtkStringObject *bit_order_str = GTK_STRING_OBJECT(
        gtk_drop_down_get_selected_item(data->settings->bit_order_dropdown));
    uint8_t bit_order = (uint8_t)strtoul(gtk_string_object_get_string(bit_order_str), NULL, 10);

    // Get SPI Mode from dropdowns:
    GtkStringObject *mode_str = GTK_STRING_OBJECT(
        gtk_drop_down_get_selected_item(data->settings->mode_dropdown));
    
    // SPI mode addresses com from Arduino SPI.h library
    // https://github.com/arduino/ArduinoCore-avr/blob/master/libraries/SPI/src/SPI.h
    const char *mode = gtk_string_object_get_string(mode_str);
    uint8_t mode_byte;
    if (strcmp(mode, "SPI_MODE0") == 0) {
        mode_byte = 0x00;
    } else if (strcmp(mode, "SPI_MODE1") == 0) {
        mode_byte = 0x04;
    } else if (strcmp(mode, "SPI_MODE2") == 0) {
        mode_byte = 0x08;
    } else { // SPI_MODE3
        mode_byte = 0x0C;
    }

    // Safety check to ensure connection information is wired correctly
    if (!data->conn) {
        g_printerr("Error: Connection data not linked to SPI section.\n");
        return;
    }

    int bytes_to_print = 0;
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(data->radio_btn0))) {
        bytes_to_print = 1;
    }
    else if (gtk_check_button_get_active(GTK_CHECK_BUTTON(data->radio_btn1))) {
        bytes_to_print = 2;
    }
    else if (gtk_check_button_get_active(GTK_CHECK_BUTTON(data->radio_btn2))) {
        bytes_to_print = 3;
    }

    // Build the binary message: [0x02][count][data byte(s)...]
    char msg[5];
    msg[0] = 0x02;
    msg[1] = (char)bytes_to_print;

    for (int i = 0; i < bytes_to_print; i++) {
        const char *hex_val = gtk_editable_get_text(GTK_EDITABLE(data->byte_entries[i]));
        long val = strtol(hex_val, NULL, 16);   // parses "A5", "0xA5", "3F", etc.
        msg[2 + i] = (char)(val & 0xFF);
    }

    size_t msg_len = (size_t)(2 + bytes_to_print);

    g_print("Sending 0x:");
    for (int i = 0; i < bytes_to_print; i++) {
        g_print(" %02X", (unsigned char)msg[2 + i]);
    }
    g_print("\n");

    g_atomic_int_set(&rx_byte_index, 0);
    send_bytes(msg, msg_len);
}

// When 'update' clicked, send that voltage to the corresponding channel
static void on_update_clicked(GtkButton *button, gpointer user_data) {
    BtnCallbackData *data = (BtnCallbackData *)user_data;
    int       CH    = data->ch;
    GtkEntry *entry = GTK_ENTRY(data->entry);

    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    uint16_t v = voltageToCode(value);

    send_voltage_command(REG_DAC, CH, v);
}

// Power off the channel
static void on_off_clicked(GtkToggleButton *button, gpointer user_data) {
    BtnCallbackData *data = (BtnCallbackData *)user_data;
    int CH = data->ch;
    GtkEntry *entry = GTK_ENTRY(data->entry);
    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));

    gboolean is_on = gtk_toggle_button_get_active(button);
    set_off_button_visual(GTK_WIDGET(button), is_on);
    
    uint16_t v;
    if (is_on == TRUE) {
        v = 0x0000; // DAC power on
    } else {
        v = 0x0001; // DAC power off
    }

    send_voltage_command(REG_CONFIG, CH, v);
}

// Assuming microcontroller RST pin is active LOW, pull it down.
static void on_reset_clicked(GtkButton *button) {
    uint8_t msg[2] = {RST_CMD_START, 0x00};
    g_print("Pulling down RST pin.\n"); // "%02X %02X\n", msg[0], msg[1]
    send_bytes(msg, sizeof(msg));
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    apply_value(entry, value);
}

static void on_entry_focus_leave(GtkEventController *controller, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    apply_value(entry, value);
}

#ifndef _WIN32
// Callback triggered when the serial port has data to read (Linux/macOS)
static gboolean on_serial_data_available(GIOChannel *source, GIOCondition condition, gpointer user_data) {
    if (condition & (G_IO_IN | G_IO_PRI)) {
        int fd = g_io_channel_unix_get_fd(source);
        char buf[256];
        ssize_t bytes_read = read(fd, buf, sizeof(buf) - 1);

        if (bytes_read < 0 && errno != EAGAIN && errno != EINTR) {
            serial_watch_id = 0; // returning FALSE below removes the source
            connection_lost("device removed");
            return FALSE;
        }

        if (bytes_read > 0) {
            buf[bytes_read] = '\0';

            // Tokenize by newline to catch "RX: <byte>" lines
            char *line = strtok(buf, "\n");

            while (line != NULL) {
                unsigned int hex_val;
                // Parse the expected format: "RX: <byte>" (e.g., "RX: 0xAA" or "RX: AA")
                if (sscanf(line, "RX: %x", &hex_val) == 1 || sscanf(line, "RX: 0x%x", &hex_val) == 1) {
                    int byte_index = g_atomic_int_get(&rx_byte_index);
                    if (global_rx_widgets && byte_index < 3) {
                        char markup[64];
                        snprintf(markup, sizeof(markup), "<b>0x%02X</b>", hex_val);
                        gtk_label_set_markup(GTK_LABEL(global_rx_widgets->received_labels[byte_index]), markup);

                        g_atomic_int_set(&rx_byte_index, (byte_index + 1) % 3);
                    }
                }
                line = strtok(NULL, "\n");
            }
        }
    }

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        serial_watch_id = 0; // returning FALSE below removes the source
        connection_lost("device removed");
        return FALSE;
    }

    return TRUE; // Continue monitoring
}
#endif

#ifdef _WIN32

// Forward-declare the payload struct at file scope so both functions can see it
typedef struct {
    int index;
    unsigned int val;
} UpdatePayload;

// Named idle callback — replaces the C++ lambda
static gboolean update_rx_label_cb(gpointer data) {
    UpdatePayload *p = (UpdatePayload *)data;
    if (global_rx_widgets) {
        char markup[64];
        snprintf(markup, sizeof(markup), "<b>0x%02X</b>", p->val);
        gtk_label_set_markup(GTK_LABEL(global_rx_widgets->received_labels[p->index]), markup);
    }
    g_free(p);
    return FALSE; // Run once
}

// Structure to pass connection info to the thread
typedef struct {
    HANDLE hPort;
    guint  generation;
} WinThreadData;

// Runs on the main loop when the reader notices the port died
static gboolean win_connection_lost_idle(gpointer data) {
    if (GPOINTER_TO_UINT(data) == conn_generation && !serial_is_invalid(active_serial_fd))
        connection_lost("device removed");
    return FALSE;
}

// Background thread function for Windows serial parsing
static gpointer windows_serial_thread_func(gpointer user_data) {
    WinThreadData *tdata = (WinThreadData *)user_data;
    HANDLE hPort = tdata->hPort;
    guint generation = tdata->generation;
    g_free(tdata);

    char rx_buf[512];
    DWORD bytes_read;

    while (!g_atomic_int_get(&win_reader_stop)) {
        if (!ReadFile(hPort, rx_buf, sizeof(rx_buf) - 1, &bytes_read, NULL)) {
            g_idle_add(win_connection_lost_idle, GUINT_TO_POINTER(generation));
            break;
        }
        if (bytes_read > 0) {
            rx_buf[bytes_read] = '\0';

            char *line = strtok(rx_buf, "\n");
            while (line != NULL) {
                unsigned int hex_val;
                if (sscanf(line, "RX: %x", &hex_val) == 1 || sscanf(line, "RX: 0x%x", &hex_val) == 1) {

                    int byte_index = g_atomic_int_get(&rx_byte_index);
                    UpdatePayload *payload = g_new(UpdatePayload, 1);
                    payload->index = byte_index;
                    payload->val = hex_val;

                    // Pass the named function instead of the lambda
                    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, update_rx_label_cb, payload, NULL);

                    g_atomic_int_set(&rx_byte_index, (byte_index + 1) % 3);
                }
                line = strtok(NULL, "\n");
            }
        }
        g_usleep(10000);
    }
    return NULL;
}
#endif

// Human-readable reason the last open_serial() failed, with a fix where we know one
static const char *serial_open_error(char *buf, size_t n) {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED)
        snprintf(buf, n, "port is busy (is another program using it?)");
    else if (err == ERROR_FILE_NOT_FOUND)
        snprintf(buf, n, "port not found (was the board unplugged?)");
    else
        snprintf(buf, n, "Windows error %lu", (unsigned long)err);
#else
    if (errno == EACCES)
        snprintf(buf, n, "permission denied (on Linux add yourself to the 'dialout' group)");
    else if (errno == EBUSY)
        snprintf(buf, n, "port is busy (is another program using it?)");
    else
        snprintf(buf, n, "%s", strerror(errno));
#endif
    return buf;
}

// Opens the selected port and starts the reader. Replaces any existing connection.
static gboolean connect_serial(ConnectData *data) {
    const char *port_sel, *baud;
    if (!get_port_and_baud(data, &port_sel, &baud)) {
        set_status("No serial port selected. Plug in the board and press Refresh.");
        return FALSE;
    }
    char *port = g_strdup(port_sel); // the dropdown's string can change under us

    disconnect_serial();
    g_print("Connecting on %s at %s baud...\n", port, baud);

    serial_fd_t fd = open_serial(port, baud_from_string(baud));
    if (serial_is_invalid(fd)) {
        char why[160];
        set_status("Could not open %s: %s", port, serial_open_error(why, sizeof(why)));
        g_printerr("Could not open %s: %s\n", port, why);
        g_free(port);
        return FALSE;
    }
    active_serial_fd = fd;

    // Hook up asynchronous reading based on OS
#ifdef _WIN32
    WinThreadData *tdata = g_new(WinThreadData, 1);
    tdata->hPort = fd;
    tdata->generation = conn_generation;
    g_atomic_int_set(&win_reader_stop, 0);
    win_reader = g_thread_new("windows_serial_reader", windows_serial_thread_func, tdata);
#else
    GIOChannel *channel = g_io_channel_unix_new(fd);
    serial_watch_id = g_io_add_watch(channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
                                     on_serial_data_available, NULL);
    g_io_channel_unref(channel); // The watch keeps its own reference now
#endif

    set_status("Connected: %s @ %s baud", port, baud);
    g_print("Port %s opened successfully and listening.\n", port);
    g_free(port);
    return TRUE;
}

// Rebuilds the port dropdown from what is plugged in right now. If exactly one
// Feather-looking port is present and we aren't connected, connects to it.
static void refresh_ports(ConnectData *data) {
    GPtrArray *ports = enumerate_ports();
    GtkStringList *list = GTK_STRING_LIST(gtk_drop_down_get_model(data->com_dropdown));
    gboolean connected = !serial_is_invalid(active_serial_fd);

    char *prev = NULL;
    GtkStringObject *sel = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(data->com_dropdown));
    if (sel) prev = g_strdup(gtk_string_object_get_string(sel));

    const char **names = g_new0(const char *, ports->len + 1);
    guint feathers = 0, feather_idx = 0, prev_idx = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < ports->len; i++) {
        PortInfo *info = g_ptr_array_index(ports, i);
        names[i] = info->path;
        if (info->is_feather && feathers++ == 0) feather_idx = i;
        if (prev && strcmp(prev, info->path) == 0) prev_idx = i;
    }

    gtk_string_list_splice(list, 0, g_list_model_get_n_items(G_LIST_MODEL(list)), names);

    if (ports->len > 0) {
        guint pick = 0;
        if (connected && prev_idx != GTK_INVALID_LIST_POSITION) pick = prev_idx;
        else if (feathers > 0) pick = feather_idx;
        else if (prev_idx != GTK_INVALID_LIST_POSITION) pick = prev_idx;
        gtk_drop_down_set_selected(data->com_dropdown, pick);
    }

    if (!connected) {
        if (ports->len == 0)
            set_status("No serial ports found. Plug in the board and press Refresh.");
        else if (feathers == 1)
            connect_serial(data);
        else
            set_status("Select a port and press Connect.");
    }

    g_free(names);
    g_free(prev);
    g_ptr_array_free(ports, TRUE);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    refresh_ports((ConnectData *)user_data);
}

static void on_connect_clicked(GtkButton *button, gpointer user_data) {
    connect_serial((ConnectData *)user_data);
}

// Close connection
static void on_disconnect_clicked(GtkButton *button, gpointer user_data) {
    if (serial_is_invalid(active_serial_fd)) {
        set_status("Not connected.");
        return;
    }
    disconnect_serial();
    set_status("Disconnected.");
    g_print("Serial connection closed.\n");
}

static void on_window_destroy(GtkWidget *window, gpointer user_data) {
    disconnect_serial();
}

// Change toggle 'on/off' state without running callback
static void set_toggle_silently(GtkToggleButton *tb, BtnCallbackData *off_data, gboolean is_on) {
    g_signal_handlers_block_by_func(tb, G_CALLBACK(on_off_clicked), off_data);
    gtk_toggle_button_set_active(tb, is_on);
    set_off_button_visual(GTK_WIDGET(tb), is_on);
    g_signal_handlers_unblock_by_func(tb, G_CALLBACK(on_off_clicked), off_data);
}

// Powers every channel on or off: sends the DAC power-down write, then updates the toggle.
// Does nothing (buttons keep their state) if the write can't be sent.
static void set_all_power(AllBtnData *data, gboolean is_on) {
    if (!require_connection()) return;
    for (int i = 0; i < 12; i++) {
        if (!send_voltage_command(REG_CONFIG, i, is_on ? 0x0000 : 0x0001)) return;
        set_toggle_silently(data->off_buttons[i], data->off_data[i], is_on);
    }
}

static void on_alloff_clicked(GtkButton *button, gpointer user_data) {
    set_all_power((AllBtnData *)user_data, FALSE);
}

static void on_allon_clicked(GtkButton *button, gpointer user_data) {
    set_all_power((AllBtnData *)user_data, TRUE);
}

// Opens the serial port and sends a voltage command for every channel
static void on_updateall_clicked(GtkButton *button, gpointer user_data) {
    AllBtnData *data = (AllBtnData *)user_data;

    if (!require_connection()) return;

    // Send one command per channel over the single open connection
    for (int i = 0; i < 12; i++) {
        GtkEntry *entry = GTK_ENTRY(data->entry_boxes[i]);
        double value    = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
        uint16_t v = voltageToCode(value);

        if (!send_voltage_command(REG_DAC, i, v)) return; //data->channel_values[i]
    }
}

// Presets: named sets of channel voltages kept in PRESET_FILE (GKeyFile format), e.g.
//   [Smoke Test]
//   ch0 (DISCBIAS)=1.2
//   ch5=2.5
// A key is "ch<N>" optionally followed by a label such as " (DISCBIAS)", which the GUI ignores
// when reading. Channels a preset doesn't list are left alone. Edit the file by hand or use "Save current".
#define PRESET_FILE "presets.ini"

typedef struct {
    AllBtnData  *all;
    GtkDropDown *dropdown;
    GtkWidget   *name_entry;
} PresetData;

static GKeyFile *presets_open(void) {
    GKeyFile *kf = g_key_file_new();
    // A missing file just means no presets yet
    g_key_file_load_from_file(kf, PRESET_FILE, G_KEY_FILE_KEEP_COMMENTS, NULL);
    return kf;
}

// Returns the key in `group` that refers to channel `ch` ("ch3" or "ch3 (READBIAS1)"), or NULL
static char *preset_key_for_channel(GKeyFile *kf, const char *group, int ch) {
    gchar **keys = g_key_file_get_keys(kf, group, NULL, NULL);
    char *found = NULL;
    for (int i = 0; keys && keys[i] && !found; i++) {
        int num, n = 0;
        if (sscanf(keys[i], "ch%d%n", &num, &n) < 1) continue;
        if (num == ch && (keys[i][n] == '\0' || keys[i][n] == ' ' || keys[i][n] == '('))
            found = g_strdup(keys[i]);
    }
    g_strfreev(keys);
    return found;
}

// Shows "Channel N (NAME):" when the preset key carries a label like "ch3 (READBIAS1)"
static void set_channel_label(GtkWidget *label, int ch, const char *key) {
    const char *open = strchr(key, '(');
    const char *close = open ? strrchr(open, ')') : NULL;
    char *text;
    if (open && close && close > open + 1)
        text = g_strdup_printf("Channel %d (%.*s):", ch, (int)(close - open - 1), open + 1);
    else
        text = g_strdup_printf("Channel %d:", ch);
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
}

// Rebuilds the preset dropdown from the file, selecting `select_name` if it exists
static void presets_reload(PresetData *pd, const char *select_name) {
    GKeyFile *kf = presets_open();
    gsize n = 0;
    gchar **groups = g_key_file_get_groups(kf, &n);
    GtkStringList *list = GTK_STRING_LIST(gtk_drop_down_get_model(pd->dropdown));
    gtk_string_list_splice(list, 0, g_list_model_get_n_items(G_LIST_MODEL(list)),
                           (const char *const *)groups);
    for (gsize i = 0; select_name && i < n; i++) {
        if (strcmp(groups[i], select_name) == 0) {
            gtk_drop_down_set_selected(pd->dropdown, (guint)i);
            break;
        }
    }
    g_strfreev(groups);
    g_key_file_free(kf);
}

// Fills the voltage entries from the selected preset; with `send`, also powers those
// channels on and writes their voltages to the board.
static void preset_apply(PresetData *pd, gboolean send) {
    GtkStringObject *sel = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(pd->dropdown));
    if (!sel) {
        set_status("No preset selected. Add one to %s or use Save current.", PRESET_FILE);
        return;
    }
    if (send && !require_connection()) return;

    char *name = g_strdup(gtk_string_object_get_string(sel));
    GKeyFile *kf = presets_open();
    gboolean present[12] = { FALSE };
    int loaded = 0;

    for (int i = 0; i < 12; i++) {
        char *key = preset_key_for_channel(kf, name, i);
        if (!key) continue;
        GError *err = NULL;
        double v = g_key_file_get_double(kf, name, key, &err);
        if (err) {
            set_status("Preset \"%s\": bad value for %s", name, key);
            g_error_free(err);
            g_free(key);
            goto done; // don't send a half-read preset
        }
        set_channel_label(pd->all->channel_labels[i], i, key);
        g_free(key);
        apply_value(GTK_ENTRY(pd->all->entry_boxes[i]), v);
        present[i] = TRUE;
        loaded++;
    }

    if (send) {
        for (int i = 0; i < 12; i++) {
            if (!present[i]) continue;
            double v = atof(gtk_editable_get_text(GTK_EDITABLE(pd->all->entry_boxes[i])));
            if (!send_voltage_command(REG_CONFIG, i, 0x0000) ||  // DAC power on
                !send_voltage_command(REG_DAC, i, voltageToCode(v)))
                goto done;
            set_toggle_silently(pd->all->off_buttons[i], pd->all->off_data[i], TRUE);
        }
    }
    set_status("Preset \"%s\": %d channels %s", name, loaded, send ? "sent" : "loaded (not sent)");

done:
    g_key_file_free(kf);
    g_free(name);
}

static void on_preset_load_clicked(GtkButton *button, gpointer user_data) {
    preset_apply((PresetData *)user_data, FALSE);
}

static void on_preset_send_clicked(GtkButton *button, gpointer user_data) {
    preset_apply((PresetData *)user_data, TRUE);
}

// Stores the current voltage entries under the name typed in the preset name box
static void on_preset_save_clicked(GtkButton *button, gpointer user_data) {
    PresetData *pd = (PresetData *)user_data;
    char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(pd->name_entry))));
    if (name[0] == '\0' || strpbrk(name, "[]") || g_ascii_iscntrl(name[0])) {
        set_status("Type a preset name (no square brackets) to save.");
        g_free(name);
        return;
    }

    GKeyFile *kf = presets_open();
    // Reuse the key (with its label) already in this preset, else the first preset's labels
    gchar **groups = g_key_file_get_groups(kf, NULL);
    const char *label_group = groups && groups[0] ? groups[0] : NULL;
    for (int i = 0; i < 12; i++) {
        char val[32];
        char *key = preset_key_for_channel(kf, name, i);
        if (!key && label_group) key = preset_key_for_channel(kf, label_group, i);
        if (!key) key = g_strdup_printf("ch%d", i);
        double v = snap_and_clamp(atof(gtk_editable_get_text(GTK_EDITABLE(pd->all->entry_boxes[i]))));
        snprintf(val, sizeof(val), "%.4f", v);
        g_key_file_set_value(kf, name, key, val);
        g_free(key);
    }
    g_strfreev(groups);

    GError *err = NULL;
    if (g_key_file_save_to_file(kf, PRESET_FILE, &err)) {
        set_status("Saved preset \"%s\" to %s", name, PRESET_FILE);
        presets_reload(pd, name);
    } else {
        set_status("Could not write %s: %s", PRESET_FILE, err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
    g_free(name);
}

// When the byte entry value is changed, check if it's a valid hex value
gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
	guint keycode, GdkModifierType state, gpointer user_date) {
	
	GtkWidget *entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
	GtkEditable *editable = GTK_EDITABLE(entry);
	
    // Allow standard navigation and control keys (Backspace, Delete, Left, Right, Tab, Enter)
    if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete ||
        keyval == GDK_KEY_Left      || keyval == GDK_KEY_Right  ||
        keyval == GDK_KEY_Tab       || keyval == GDK_KEY_Return || 
        keyval == GDK_KEY_KP_Enter) {
        return FALSE; // Let GTK handle these standard text actions
    }

    // Convert keyval to a Unicode character. 
    // If it returns 0, it's a non-text key (like a standalone Ctrl, Alt, Shift, or F1-F12 key).
    guint32 key_char = gdk_keyval_to_unicode(keyval);
    if (key_char == 0) {
        return FALSE; // Pass it through safely
    }
    
    // Validate that the text character is a hex digit
    if (!isxdigit((unsigned char)key_char)) {
        gtk_widget_error_bell(entry); 
        return TRUE; // Block invalid character
    }	
	
	return FALSE; // Accept the valid hex character
}

// Dark theme: black and grey, with purple used sparingly (focus, selection, ON state, checks).
// Named buttons (btn-on / btn-off) come from set_off_button_visual().
static const char *const THEME_CSS =
    "window, scrolledwindow, viewport { background-color: #0f0f10; color: #d8d8dc; }\n"
    "label { color: #d8d8dc; }\n"
    "separator { background-color: #2e2e33; min-height: 1px; }\n"
    "entry { background: #1c1c20; color: #e8e8ec; border: 1px solid #2e2e33; border-radius: 6px;"
    "       caret-color: #a58ee6; padding: 4px 8px; }\n"
    "entry:focus-within { border-color: #8b6fd6; box-shadow: none; }\n"
    "entry selection { background-color: #4a3d78; color: #ffffff; }\n"
    "button { background: #1c1c20; color: #d8d8dc; border: 1px solid #2e2e33; border-radius: 6px;"
    "        box-shadow: none; text-shadow: none; padding: 4px 12px; }\n"
    "button:hover { background: #26262b; border-color: #4a4a52; }\n"
    "button:active { background: #2e2e33; }\n"
    "button:disabled { color: #5c5c64; }\n"
    "button#btn-on { background: #1f1b2e; border-color: #6f58b8; color: #c9bdf0; }\n"
    "button#btn-on:hover { background: #262138; }\n"
    "button#btn-off { background: #151517; border-color: #2e2e33; color: #6e6e76; }\n"
    "dropdown button { background: #1c1c20; }\n"
    "popover contents, popover arrow { background: #17171a; color: #d8d8dc; border: 1px solid #2e2e33; }\n"
    "popover row:hover { background: #26262b; }\n"
    "popover row:selected { background: #2e2e33; }\n"
    "check { background: #1c1c20; border: 1px solid #3a3a40; }\n"
    "check:checked { background: #6f58b8; border-color: #8b6fd6; color: #ffffff; }\n"
    "scrollbar slider { background: #34343a; border-radius: 6px; min-width: 8px; min-height: 8px; }\n"
    "scrollbar slider:hover { background: #4a4a52; }\n";

static void apply_theme(void) {
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
#if GTK_CHECK_VERSION(4, 20, 0)
        g_object_set(settings, "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
#else
        g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, NULL);
#endif
    }

    GtkCssProvider *provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, THEME_CSS);
#else
    gtk_css_provider_load_from_data(provider, THEME_CSS, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// Initialize GUI widgets inside activate
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window;

    apply_theme();
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Reference Voltage Generator");
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    // Open as wide as the widest row (the preset row), not a hardcoded width
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_window_set_child(GTK_WINDOW(window), scroll);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), vbox);

    // Serial connection row (built first so conn_data exists for channels)
    GtkWidget *conn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(vbox), conn_box);

    GtkWidget *com_label = gtk_label_new("COM Port:");
    gtk_widget_set_halign(com_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(conn_box), com_label);

    // Filled in by refresh_ports() once the rest of the UI exists
    GtkWidget *com_dropdown = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL);
    gtk_box_append(GTK_BOX(conn_box), com_dropdown);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_box_append(GTK_BOX(conn_box), refresh_btn);

    GtkWidget *baud_label = gtk_label_new("Baud Rate:");
    gtk_widget_set_halign(baud_label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(baud_label, 8);
    gtk_box_append(GTK_BOX(conn_box), baud_label);

    const char *baud_rates[] = {
        "1200", "2400", "4800", "9600",
        "19200", "38400", "57600", "115200",
        "230400", "460800", "921600",
        NULL
    };
    GtkStringList *baud_list = gtk_string_list_new(baud_rates);
    GtkWidget *baud_dropdown = gtk_drop_down_new(G_LIST_MODEL(baud_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(baud_dropdown), 7); // 115200
    gtk_box_append(GTK_BOX(conn_box), baud_dropdown);

    GtkWidget *connect_btn = gtk_button_new_with_label("Connect");
    GtkWidget *disconnect_btn = gtk_button_new_with_label("Disconnect");
    gtk_widget_set_margin_start(connect_btn, 8);
    gtk_widget_set_margin_start(disconnect_btn, 8);

    ConnectData *conn_data = g_new0(ConnectData, 1);
    conn_data->com_dropdown  = GTK_DROP_DOWN(com_dropdown);
    conn_data->baud_dropdown = GTK_DROP_DOWN(baud_dropdown);
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect_clicked), conn_data);
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), NULL);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), conn_data);
    gtk_box_append(GTK_BOX(conn_box), connect_btn);
    gtk_box_append(GTK_BOX(conn_box), disconnect_btn);

    // Connection status line, then populate the port list (auto-connects to a lone Feather)
    status_label = GTK_LABEL(gtk_label_new("Disconnected"));
    gtk_widget_set_halign(GTK_WIDGET(status_label), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(status_label));
    refresh_ports(conn_data);

    // Channel grid with channel number, voltage entry, increase/decrease, etc
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(vbox), grid);
 
    GtkWidget       *entry_boxes[12];
    GtkWidget       *channel_labels[12];
    GtkToggleButton *off_buttons[12];
    BtnCallbackData *off_cb_data_arr[12]; // one BtnCallbackData* per off button
    GtkWidget       *update_buttons[12];

    for (int i = 0; i < 12; i++) {
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "Channel %d:", i);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), label, 0, i, 1, 1);
        channel_labels[i] = label;

        GtkWidget *entry = gtk_entry_new();
        apply_value(GTK_ENTRY(entry), 0.0);
        gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_NUMBER);
        gtk_widget_set_hexpand(entry, TRUE);

        g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), NULL);
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(on_entry_focus_leave), entry);
        gtk_widget_add_controller(entry, focus);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, i, 1, 1);

        entry_boxes[i] = entry;

        // column 2: increase voltage value
        GtkWidget *up_btn = gtk_button_new_from_icon_name("go-up-symbolic");
        gtk_widget_set_tooltip_text(up_btn, "Increase value (+100 µV)");
        g_signal_connect(up_btn, "clicked", G_CALLBACK(on_up_clicked), entry_boxes[i]);
        gtk_grid_attach(GTK_GRID(grid), up_btn, 2, i, 1, 1);

        // column 3: decrease voltage value
        GtkWidget *down_btn = gtk_button_new_from_icon_name("go-down-symbolic");
        gtk_widget_set_tooltip_text(down_btn, "Decrease value (-100 µV)");
        g_signal_connect(down_btn, "clicked", G_CALLBACK(on_down_clicked), entry_boxes[i]);
        gtk_grid_attach(GTK_GRID(grid), down_btn, 3, i, 1, 1);

        // column 4: power off channel
        // The firmware powers every DAC on at startup, so the button starts as ON
        GtkToggleButton *off_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("ON"));
        gtk_toggle_button_set_active(off_btn, TRUE);
        set_off_button_visual(GTK_WIDGET(off_btn), TRUE);
        gtk_widget_set_tooltip_text(GTK_WIDGET(off_btn), "Set channel to 0 V");
        BtnCallbackData *off_cb_data = make_btn_data(i, entry_boxes[i], conn_data);
        g_signal_connect(off_btn, "clicked", G_CALLBACK(on_off_clicked), off_cb_data);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(off_btn), 4, i, 1, 1);
        off_buttons[i]      = off_btn;
        off_cb_data_arr[i]  = off_cb_data; // save for AllBtnData below

        // column 5: send voltage value to serial port
        BtnCallbackData *cb_data = make_btn_data(i, entry_boxes[i], conn_data); 
        GtkWidget *update_btn = gtk_button_new_with_label("UPDATE");
        g_signal_connect(update_btn, "clicked", G_CALLBACK(on_update_clicked), cb_data);
        gtk_grid_attach(GTK_GRID(grid), update_btn, 5, i, 1, 1);
        update_buttons[i] = update_btn;
    }

    gtk_window_set_default_size(GTK_WINDOW(window), -1, 950);
    gtk_window_present(GTK_WINDOW(window));

    // power off all channels, power on all channels, update all channels
    GtkWidget *all_off_btn = gtk_button_new_with_label("All OFF");
    GtkWidget *all_on_btn = gtk_button_new_with_label("All ON");
    GtkWidget *all_update_btn = gtk_button_new_with_label("All UPDATE");

    /* Allocate and populate AllBtnData so the three bulk callbacks can reach
     * the per-channel widgets and the shared serial dropdowns. */
    AllBtnData *all_data = g_new0(AllBtnData, 1);
    all_data->conn = conn_data;
    for (int i = 0; i < 12; i++) {
        all_data->off_buttons[i]    = off_buttons[i];
        all_data->off_data[i]       = off_cb_data_arr[i];
        all_data->entry_boxes[i]    = entry_boxes[i];
        all_data->channel_labels[i] = channel_labels[i];
    }

    g_signal_connect(all_off_btn,    "clicked", G_CALLBACK(on_alloff_clicked),    all_data);
    g_signal_connect(all_on_btn,     "clicked", G_CALLBACK(on_allon_clicked),     all_data);
    g_signal_connect(all_update_btn, "clicked", G_CALLBACK(on_updateall_clicked), all_data);

    // Horizontal button bar
    GtkWidget *all_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_hexpand(all_box, TRUE);
	gtk_widget_set_halign(all_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(all_box), all_off_btn);
    gtk_box_append(GTK_BOX(all_box), all_on_btn);
    gtk_box_append(GTK_BOX(all_box), all_update_btn);
    gtk_box_append(GTK_BOX(vbox), all_box);

    // Preset row: pick a saved preset to load or send, or save the current values
    PresetData *preset_data = g_new0(PresetData, 1);
    preset_data->all = all_data;
    preset_data->dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)), NULL));
    preset_data->name_entry = gtk_entry_new();
    g_object_set(preset_data->name_entry, "placeholder-text", "New preset name", NULL);
    gtk_widget_set_hexpand(preset_data->name_entry, TRUE);

    GtkWidget *preset_load_btn = gtk_button_new_with_label("Load");
    GtkWidget *preset_send_btn = gtk_button_new_with_label("Load & Send");
    GtkWidget *preset_save_btn = gtk_button_new_with_label("Save current");
    gtk_widget_set_tooltip_text(preset_load_btn, "Fill the voltage boxes without sending");
    gtk_widget_set_tooltip_text(preset_send_btn, "Fill the voltage boxes, power the channels on and send them");
    g_signal_connect(preset_load_btn, "clicked", G_CALLBACK(on_preset_load_clicked), preset_data);
    g_signal_connect(preset_send_btn, "clicked", G_CALLBACK(on_preset_send_clicked), preset_data);
    g_signal_connect(preset_save_btn, "clicked", G_CALLBACK(on_preset_save_clicked), preset_data);

    GtkWidget *preset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(preset_box), gtk_label_new("Preset:"));
    gtk_box_append(GTK_BOX(preset_box), GTK_WIDGET(preset_data->dropdown));
    gtk_box_append(GTK_BOX(preset_box), preset_load_btn);
    gtk_box_append(GTK_BOX(preset_box), preset_send_btn);
    gtk_box_append(GTK_BOX(preset_box), preset_data->name_entry);
    gtk_box_append(GTK_BOX(preset_box), preset_save_btn);
    gtk_box_append(GTK_BOX(vbox), preset_box);
    presets_reload(preset_data, NULL);
	
	// SPI Communication section
	GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_margin_top(sep2, 8);
	gtk_widget_set_margin_bottom(sep2, 8);
	gtk_box_append(GTK_BOX(vbox), sep2);
	
	// SPI section title
	GtkWidget *spi_title = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(spi_title), "<b>SPI Communication</b>");
	gtk_box_append(GTK_BOX(vbox), spi_title);

    // Select SPI speed
    GtkWidget *speed_label = gtk_label_new("Hz");
    const char *spi_speeds[] = {"1000000", NULL};
    GtkStringList *speed_list = gtk_string_list_new(spi_speeds);
    GtkWidget *speed_dropdown = gtk_drop_down_new(G_LIST_MODEL(speed_list), NULL);

    // Select MSB or LSB first
    GtkWidget *bit_label = gtk_label_new("MSB or LSB");
    const char *first_bit[] = {"MSBFIRST","LSBFIRST", NULL};
    GtkStringList *bit_list = gtk_string_list_new(first_bit);
    GtkWidget *bit_dropdown = gtk_drop_down_new(G_LIST_MODEL(bit_list), NULL);

    // Select SPI mode
    GtkWidget *mode_label = gtk_label_new("Mode");
    const char *modes[] = {"SPI_MODE0", "SPI_MODE1", "SPI_MODE2", "SPI_MODE3", NULL};
    GtkStringList *mode_list = gtk_string_list_new(modes);
    GtkWidget *mode_dropdown = gtk_drop_down_new(G_LIST_MODEL(mode_list), NULL);

    // Write to Reset pin (any GPIO on a microcontroller)
    GtkWidget *reset_btn = gtk_button_new_with_label("RESET");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked), NULL);

    // SPI settings bar
    GtkWidget *spi_settings = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_hexpand(spi_settings, TRUE);
    gtk_widget_set_halign(spi_settings, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(spi_settings), speed_label);
    gtk_box_append(GTK_BOX(spi_settings), speed_dropdown);
    gtk_box_append(GTK_BOX(spi_settings), bit_label);
    gtk_box_append(GTK_BOX(spi_settings), bit_dropdown);
    gtk_box_append(GTK_BOX(spi_settings), mode_label);
    gtk_box_append(GTK_BOX(spi_settings), mode_dropdown);
    gtk_box_append(GTK_BOX(spi_settings), reset_btn);
    gtk_box_append(GTK_BOX(vbox), spi_settings);
	
	// SPI byte grid
	GtkWidget *spi_grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(spi_grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(spi_grid), 8);
	gtk_box_append(GTK_BOX(vbox), spi_grid);

	// 1st row: labels for byte 0, 1, 2
	GtkWidget *byte_labels[3];
	for (int i=0; i<3; i++) {

		// create center-aligned label in format "byte <i>"
		char label_text[32];
		snprintf(label_text, sizeof(label_text), "Byte %d", i);
		GtkWidget *label = gtk_label_new(label_text);
		gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
		gtk_grid_attach(GTK_GRID(spi_grid), label, i, 0, 1, 1); // column i, row 0

		// save to array
		byte_labels[i] = label;
	}

	// 2nd row: entry boxes for byte 0, 1, 2
	GtkWidget *byte_entries[3];
	for (int i=0; i<3; i++) {
		
		// create center-aligned entry box that takes 2 hex digits
		GtkWidget *byte_entry = gtk_entry_new();
		
		// limit entry to 2 characters
		gtk_entry_set_max_length(GTK_ENTRY(byte_entry), 2);
		
		// widget width and grid placement	
		gtk_widget_set_hexpand(byte_entry, TRUE);
		gtk_grid_attach(GTK_GRID(spi_grid), byte_entry, i, 1, 1, 1); // column i, row 1
		
		// save to array
		byte_entries[i] = byte_entry;
	}
	
	// byte select
	GtkWidget *send_label = gtk_label_new("Select which bytes to send.");
	gtk_widget_set_margin_top(send_label, 12);
	gtk_box_append(GTK_BOX(vbox), send_label);
	
	// Checkbuttons acquire the behavior of radio buttons when grouped	
	GtkWidget *byte_radio_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(vbox), byte_radio_box);
	
	GtkWidget *byte_radio_btn0 = gtk_check_button_new_with_label("Byte 0");
	GtkWidget *byte_radio_btn1 = gtk_check_button_new_with_label("Bytes 0-1");
	GtkWidget *byte_radio_btn2 = gtk_check_button_new_with_label("Bytes 0-2");
	
	gtk_check_button_set_group(GTK_CHECK_BUTTON(byte_radio_btn1), GTK_CHECK_BUTTON(byte_radio_btn0));
	gtk_check_button_set_group(GTK_CHECK_BUTTON(byte_radio_btn2), GTK_CHECK_BUTTON(byte_radio_btn0));
	
	gtk_box_append(GTK_BOX(byte_radio_box), byte_radio_btn0);
	gtk_box_append(GTK_BOX(byte_radio_box), byte_radio_btn1);
	gtk_box_append(GTK_BOX(byte_radio_box), byte_radio_btn2);
	gtk_widget_set_halign(byte_radio_box, GTK_ALIGN_CENTER);
	
	// Add btn data to struct	
	RadioBtnSelect *radio_btn_data = g_new0(RadioBtnSelect, 1);
	radio_btn_data->radio_btn0 = byte_radio_btn0;
	radio_btn_data->radio_btn1 = byte_radio_btn1;
	radio_btn_data->radio_btn2 = byte_radio_btn2;
    for (int i=0; i<3; i++) {
        radio_btn_data->byte_entries[i] = byte_entries[i];
    }
    radio_btn_data->conn = conn_data;

    // Add SPI settings data to struct
    SPISettingsData *settings_data = g_new0(SPISettingsData, 1);
    settings_data->speed_dropdown = GTK_DROP_DOWN(speed_dropdown);
    settings_data->bit_order_dropdown = GTK_DROP_DOWN(bit_dropdown);
    settings_data->mode_dropdown = GTK_DROP_DOWN(mode_dropdown);
    radio_btn_data->settings = settings_data;

	// 'Send' button will send byte via SPI
	GtkWidget *send_btn = gtk_button_new_with_label("SEND");
	gtk_box_append(GTK_BOX(byte_radio_box), send_btn);

    // 'Send' callback
	g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_clicked), radio_btn_data);

	// Clean up struct data when send btn is destroyed
	g_signal_connect_swapped(send_btn, "destroy", G_CALLBACK(g_free), radio_btn_data);

    // Bytes-received section: Labels will update to reflect bytes received via SPI
    GtkWidget *received_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(received_title), "<b>Bytes Received</b>");
    gtk_box_append(GTK_BOX(vbox), received_title);

    // Allocate the global tracking reference
    global_rx_widgets = g_new0(IncomingDataWidgets, 1);

    // Bytes-received grid
    GtkWidget *received_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(received_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(received_grid), 8);
    gtk_box_append(GTK_BOX(vbox), received_grid);

    // 1st row: labels for byte 0, 1, 2
    for (int i=0; i<3; i++) {

        // create center-aligned label in format "byte <i>"
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "Byte %d", i);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(received_grid), label, i, 0, 1, 1); // column i, row 0
    }

    // 2nd row: labels displaying byte 0, 1, 2 received
    for (int i=0; i<3; i++) {
        
        // create center-aligned entry box that takes 2 hex digits
        GtkWidget *received_byte = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(received_byte), "<i>Nothing Received</i>");
        
        // widget width and grid placement	
        gtk_widget_set_hexpand(received_byte, TRUE);
        gtk_grid_attach(GTK_GRID(received_grid), received_byte, i, 1, 1, 1); // column i, row 1
                
        // Save to global reference
        global_rx_widgets->received_labels[i] = received_byte;
    }
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.example.adjustvoltage", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
