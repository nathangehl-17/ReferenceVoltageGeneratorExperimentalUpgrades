/*
To compile and run:
gcc $(pkg-config --cflags gtk4) -o main main.c $(pkg-config --libs gtk4) -lm && ./main
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
#  include <fcntl.h>
#  include <termios.h>
#  include <unistd.h>
   typedef int serial_fd_t;
#  define SERIAL_INVALID (-1)
#endif

#define VOLTAGE_MIN   0
#define VOLTAGE_MAX   5.0
#define VOLTAGE_STEP  0.0001   /* 100 µV */

// DAC70501 registers
const uint8_t REG_SYNC = 0x01;
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
    timeouts.WriteTotalTimeoutConstant   = 500
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hPort, &timeouts);

    return hPort;
}

static void serial_write(serial_fd_t fd, const char *buf, size_t len) {
    DWORD written;
    WriteFile(fd, buf, (DWORD)len, &written, NULL);
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
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) return -1;

    struct termios tty;
    tcgetattr(fd, &tty);
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

static void serial_write(serial_fd_t fd, const char *buf, size_t len) {
    write(fd, buf, len);
}

static void serial_close(serial_fd_t fd) {
    close(fd);
}

static int serial_is_invalid(serial_fd_t fd) {
    return fd < 0;
}

#endif /* _WIN32 */

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

// Returns SERIAL_INVALID on any failure (no selection, or the port couldn't open).
static serial_fd_t open_serial_from_conn(ConnectData *conn) {
    const char *port, *baud_str;
    if (!get_port_and_baud(conn, &port, &baud_str)) {
        g_printerr("No COM port or baud rate selected.\n");
        return SERIAL_INVALID;
    }

    serial_fd_t fd = open_serial(port, baud_from_string(baud_str));
    /*if (serial_is_invalid(fd)) {
        g_printerr("Failed to open serial port: %s\n", port);
    }*/
    return fd;
}

// Sends 5 bytes: bank, chip, register, high half of value, low half of value.
static void send_voltage_command(serial_fd_t fd, uint8_t reg, int channel, uint16_t value) {
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
    serial_write(fd, (const char *)msg, sizeof(msg));
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

    /*if (serial_is_invalid(active_serial_fd)) {
        g_printerr("Error: Cannot send. Port not connected!\n");
        return;
    }*/
    serial_write(active_serial_fd, msg, msg_len);
}

// When 'update' clicked, send that voltage to the corresponding channel
static void on_update_clicked(GtkButton *button, gpointer user_data) {
    BtnCallbackData *data = (BtnCallbackData *)user_data;
    int       CH    = data->ch;
    GtkEntry *entry = GTK_ENTRY(data->entry);

    double value = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
    uint16_t v = voltageToCode(value);

    /*if (serial_is_invalid(active_serial_fd)) {
        g_printerr("Error: Cannot send. Port not connected!\n");
        return;
    }*/
    send_voltage_command(active_serial_fd, REG_DAC, CH, v);
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

    /*if (serial_is_invalid(active_serial_fd)) {
        g_printerr("Error: Cannot send. Port not connected!\n");
        return;
    }*/

    send_voltage_command(active_serial_fd, REG_CONFIG, CH, v);
}

// Assuming microcontroller RST pin is active LOW, pull it down.
static void on_reset_clicked(GtkButton *button) {
    uint8_t msg[2] = {RST_CMD_START, 0x00};
    g_print("Pulling down RST pin.\n"); // "%02X %02X\n", msg[0], msg[1]
    serial_write(active_serial_fd, (const char *)msg, sizeof(msg));
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
// Callback triggered when the serial port has data to read (Linux)
static gboolean on_serial_data_available(GIOChannel *source, GIOCondition condition, gpointer user_data) {
    if (condition & (G_IO_IN | G_IO_PRI)) {
        int fd = g_io_channel_unix_get_fd(source);
        char buf[256];
        ssize_t bytes_read = read(fd, buf, sizeof(buf) - 1);
        
        if (bytes_read > 0) {
            buf[bytes_read] = '\0';
            
            // Tokenize by newline to catch "RX: <byte>" lines
            char *line = strtok(buf, "\n");
            static int byte_index = 0; // Tracks which label to update (0, 1, or 2)
            
            while (line != NULL) {
                unsigned int hex_val;
                // Parse the expected format: "RX: <byte>" (e.g., "RX: 0xAA" or "RX: AA")
                if (sscanf(line, "RX: %x", &hex_val) == 1 || sscanf(line, "RX: 0x%x", &hex_val) == 1) {
                    if (global_rx_widgets && byte_index < 3) {
                        char markup[64];
                        snprintf(markup, sizeof(markup), "<b>0x%02X</b>", hex_val);
                        gtk_label_set_markup(GTK_LABEL(global_rx_widgets->received_labels[byte_index]), markup);
                        
                        byte_index++;
                        if (byte_index >= 3) {
                            byte_index = 0; // Reset after filling all 3 bytes
                        }
                    }
                }
                line = strtok(NULL, "\n");
            }
        }
    }
    
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_printerr("Serial connection closed or lost.\n");
        return FALSE; // Removes the watch hook
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
} WinThreadData;

// Background thread function for Windows serial parsing
static gpointer windows_serial_thread_func(gpointer user_data) {
    WinThreadData *tdata = (WinThreadData *)user_data;
    HANDLE hPort = tdata->hPort;
    g_free(tdata);

    char rx_buf[512];
    DWORD bytes_read;
    int byte_index = 0;

    while (TRUE) {
        if (ReadFile(hPort, rx_buf, sizeof(rx_buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
            rx_buf[bytes_read] = '\0';

            char *line = strtok(rx_buf, "\n");
            while (line != NULL) {
                unsigned int hex_val;
                if (sscanf(line, "RX: %x", &hex_val) == 1 || sscanf(line, "RX: 0x%x", &hex_val) == 1) {

                    UpdatePayload *payload = g_new(UpdatePayload, 1);
                    payload->index = byte_index;
                    payload->val = hex_val;

                    // Pass the named function instead of the lambda
                    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, update_rx_label_cb, payload, NULL);

                    byte_index++;
                    if (byte_index >= 3) byte_index = 0;
                }
                line = strtok(NULL, "\n");
            }
        }
        g_usleep(10000);
    }
    return NULL;
}
#endif


static void on_connect_clicked(GtkButton *button, gpointer user_data) {
    ConnectData *data = (ConnectData *)user_data;

    const char *port = "None";
    const char *baud = "None";
    get_port_and_baud(data, &port, &baud);

    // If an older connection is active, clean it up first
    if (!serial_is_invalid(active_serial_fd)) {
        g_print("Closing existing connection...\n");
        serial_close(active_serial_fd);
        active_serial_fd = SERIAL_INVALID;
    }

    g_print("Connecting on %s at %s baud...\n", port, baud);

    active_serial_fd = open_serial(port, baud_from_string(baud));
    if (serial_is_invalid(active_serial_fd)) {
        g_printerr("Warning: could not open %s — check the port.\n", port);
        return;
    }
    g_print("Port %s opened successfully and listening.\n", port);

    // Hook up asynchronous reading based on OS
#ifdef _WIN32
    WinThreadData *tdata = g_new(WinThreadData, 1);
    tdata->hPort = active_serial_fd;
    g_thread_new("windows_serial_reader", windows_serial_thread_func, tdata);
#else
    GIOChannel *channel = g_io_channel_unix_new(active_serial_fd);
    g_io_add_watch(channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP, on_serial_data_available, NULL);
    g_io_channel_unref(channel); // The main loop keeps its own reference now
#endif
}

// Close connection
static void on_disconnect_clicked(GtkButton *button, gpointer user_data) {
    g_print("Checking for existing connection to close ...\n");
    if (!serial_is_invalid(active_serial_fd)) {
        g_print("Existing connection found. Closing...\n");
        serial_close(active_serial_fd);
        active_serial_fd = SERIAL_INVALID;
        g_print("Closed.\n");
    }
    g_print("No connection to close.\n");
}

// Change toggle 'on/off' state without running callback
static void set_toggle_silently(GtkToggleButton *tb, BtnCallbackData *off_data, gboolean is_on) {
    g_signal_handlers_block_by_func(tb, G_CALLBACK(on_off_clicked), off_data);
    gtk_toggle_button_set_active(tb, is_on);
    set_off_button_visual(GTK_WIDGET(tb), is_on);
    g_signal_handlers_unblock_by_func(tb, G_CALLBACK(on_off_clicked), off_data);
}

// Change toggle buttons' state to 'off'
static void on_alloff_clicked(GtkButton *button, gpointer user_data) {
    AllBtnData *data = (AllBtnData *)user_data;
    for (int i=0; i<12; i++) {
        gtk_toggle_button_set_active(data->off_buttons[i], FALSE);
        set_off_button_visual(GTK_WIDGET(data->off_buttons[i]), FALSE);
    }
    
}

// Change toggle buttons' state to 'on'
static void on_allon_clicked(GtkButton *button, gpointer user_data) {
    AllBtnData *data = (AllBtnData *)user_data;
    for (int i=0; i<12; i++) {
        gtk_toggle_button_set_active(data->off_buttons[i], TRUE);
        set_off_button_visual(GTK_WIDGET(data->off_buttons[i]), TRUE);
    }
}

// Opens the serial port and sends a voltage command for every channel
static void on_updateall_clicked(GtkButton *button, gpointer user_data) {
    AllBtnData *data = (AllBtnData *)user_data;

    serial_fd_t fd = open_serial_from_conn(data->conn);
    if (serial_is_invalid(fd)) return;

    // Send one command per channel over the single open connection
    for (int i = 0; i < 12; i++) {
        GtkEntry *entry = GTK_ENTRY(data->entry_boxes[i]);
        double value    = atof(gtk_editable_get_text(GTK_EDITABLE(entry)));
        uint16_t v = voltageToCode(value);

        send_voltage_command(fd, REG_DAC, i, v); //data->channel_values[i]
    }

    serial_close(fd);
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

// Initialize GUI widgets inside activate
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window;
    
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Reference Voltage Generator");
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
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

    const char *com_ports[] = {
        "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8",
        "/dev/ttyUSB0", "/dev/ttyUSB1",
        "/dev/ttyACM0", "/dev/ttyACM1",
        NULL
    };
    GtkStringList *com_list = gtk_string_list_new(com_ports);
    GtkWidget *com_dropdown = gtk_drop_down_new(G_LIST_MODEL(com_list), NULL);
    // Default to /dev/ttyACM0 (index 10)
    gtk_drop_down_set_selected(GTK_DROP_DOWN(com_dropdown), 10);
    gtk_box_append(GTK_BOX(conn_box), com_dropdown);

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
    gtk_box_append(GTK_BOX(conn_box), connect_btn);
    gtk_box_append(GTK_BOX(conn_box), disconnect_btn);

    // Channel grid with channel number, voltage entry, increase/decrease, etc
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(vbox), grid);
 
    GtkWidget       *entry_boxes[12];
    GtkToggleButton *off_buttons[12];
    BtnCallbackData *off_cb_data_arr[12]; // one BtnCallbackData* per off button
    GtkWidget       *update_buttons[12];

    for (int i = 0; i < 12; i++) {
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "Channel %d:", i);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), label, 0, i, 1, 1);

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
        GtkToggleButton *off_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("OFF"));
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

    gtk_window_set_default_size(GTK_WINDOW(window), 680, 950);
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
