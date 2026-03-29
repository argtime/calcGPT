/*
 * CalcGPT v2 - TI-84 Plus CE ChatGPT Interface
 *
 * Improvements over v1:
 *  - Full keyboard input (letters AND numbers) via os_GetStringInput
 *  - Backspace, punctuation, and all standard characters supported
 *  - Descriptive error messages for USB/serial failures
 *  - App state machine: disconnected / connected / waiting for response
 *  - Scroll debounce for smooth Up/Down navigation
 *  - Page counter displayed on screen (Page X/Y)
 *  - Left arrow = jump to first page, Right arrow = jump to latest page
 *  - User's own prompt echoed into the view buffer before response
 *  - [2nd] key debounced so it cannot double-trigger
 *  - Doubled response buffer (10 pages instead of 5)
 *  - Memory safety: fixed sizeof-pointer bugs, no heap allocation in input
 */

#include <srldrvce.h>
#include <keypadc.h>
#include <stdbool.h>
#include <string.h>
#include <tice.h>
#include <ti/screen.h>
#include <ti/getcsc.h>
#include <stdio.h>

/* -----------------------------------------------
 * Constants
 * ----------------------------------------------- */
#define BLOCK_SIZE    234   /* characters per screen page (26 cols x 9 rows) */
#define MAX_PAGES     10    /* total pages kept in the circular buffer        */
#define BUFFER_SIZE   (BLOCK_SIZE * MAX_PAGES)
#define INPUT_MAX     120   /* maximum prompt length the user can type        */

/* -----------------------------------------------
 * App state
 * ----------------------------------------------- */
typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTED,
    STATE_WAITING_RESPONSE,
} app_state_t;

static app_state_t app_state = STATE_DISCONNECTED;

/* -----------------------------------------------
 * Circular response buffer
 * ----------------------------------------------- */
static char resp_buf[BUFFER_SIZE];
static int  wr_idx         = 0;   /* next write position                */
static int  last_page      = -1;  /* most-recently written page index   */
static int  total_pages    = 0;   /* how many pages contain content     */
static int  scroll_pos     = 0;   /* currently displayed page           */

static void buf_reset(void)
{
    memset(resp_buf, 0, BUFFER_SIZE);
    wr_idx      = 0;
    last_page   = -1;
    total_pages = 0;
    scroll_pos  = 0;
}

static void buf_append(const char *data)
{
    int len        = (int)strlen(data);
    int space_left = BUFFER_SIZE - wr_idx;

    if (len > space_left) {
        memcpy(&resp_buf[wr_idx], data, space_left);
        memcpy(resp_buf, &data[space_left], len - space_left);
        wr_idx = len - space_left;
    } else {
        memcpy(&resp_buf[wr_idx], data, len);
        wr_idx += len;
        if (wr_idx >= BUFFER_SIZE)
            wr_idx = 0;
    }

    last_page = (wr_idx > 0 ? wr_idx - 1 : BUFFER_SIZE - 1) / BLOCK_SIZE;
    if (total_pages <= last_page)
        total_pages = last_page + 1;
}

/* Copy one page-worth of bytes from the circular buffer into out[].
 * out must be at least BLOCK_SIZE+1 bytes. */
static void buf_get_page(int page, char *out)
{
    int start  = page * BLOCK_SIZE;
    int avail  = BUFFER_SIZE - start;

    memset(out, 0, BLOCK_SIZE + 1);
    if (avail < BLOCK_SIZE) {
        memcpy(out, &resp_buf[start], avail);
        memcpy(out + avail, resp_buf, BLOCK_SIZE - avail);
    } else {
        memcpy(out, &resp_buf[start], BLOCK_SIZE);
    }
}

/* -----------------------------------------------
 * Display helpers
 * ----------------------------------------------- */

/* Overwrite the last screen row with a status line. */
static void draw_status_bar(void)
{
    char bar[28];
    os_SetCursorPos(9, 0);

    if (app_state == STATE_WAITING_RESPONSE) {
        snprintf(bar, sizeof(bar), "Waiting... [CLR]=Quit       ");
    } else if (app_state == STATE_CONNECTED && last_page >= 0) {
        snprintf(bar, sizeof(bar), "Pg%d/%d [2nd]=Chat [CLR]=Quit",
                 scroll_pos + 1, total_pages);
    } else if (app_state == STATE_CONNECTED) {
        snprintf(bar, sizeof(bar), "[2nd]=Chat        [CLR]=Quit");
    } else {
        snprintf(bar, sizeof(bar), "Waiting for USB connection...");
    }

    os_PutStrFull(bar);
    os_SetCursorPos(0, 0);
}

/* Render the welcome / status screen (no chat history yet). */
static void show_status_screen(void)
{
    os_ClrHome();
    os_FontSelect(os_SmallFont);
    os_SetCursorPos(0, 0);

    if (app_state == STATE_DISCONNECTED) {
        os_PutStrFull("CalcGPT v2\n\n"
                      "Connect calculator\n"
                      "to computer via USB\n"
                      "then run calcgpt.py\n\n"
                      "Waiting for USB...");
    } else if (app_state == STATE_CONNECTED) {
        os_PutStrFull("CalcGPT v2  Connected!\n\n"
                      "Press [2nd] to type a\n"
                      "prompt. Numbers and\n"
                      "letters both work.\n\n"
                      "Up/Down = scroll\n"
                      "Left = first page\n"
                      "Right = latest page\n"
                      "[CLR] = quit");
    } else {
        os_PutStrFull("Sent! Waiting for\n"
                      "ChatGPT response...\n\n"
                      "Please wait.");
    }

    draw_status_bar();
}

/* Render a page of the response buffer with the status bar. */
static void show_page(int page)
{
    char block[BLOCK_SIZE + 1];
    buf_get_page(page, block);
    scroll_pos = page;

    os_ClrHome();
    os_FontSelect(os_SmallFont);
    os_SetCursorPos(0, 0);
    os_PutStrFull(block);
    draw_status_bar();
    os_SetCursorPos(0, 0);
}

/* -----------------------------------------------
 * Input via the built-in TIOS string editor.
 * Supports all keys: letters, numbers, symbols,
 * backspace — everything the OS keyboard handles.
 * Returns true if the user entered something,
 * false if the buffer is empty (user pressed Enter
 * on a blank input).
 * ----------------------------------------------- */
static bool take_input(char *out, size_t out_size)
{
    memset(out, 0, out_size);
    os_ClrHome();
    os_FontSelect(os_SmallFont);

    /* os_GetStringInput shows prompt, lets user type, handles backspace,
     * alpha-lock, number keys, and all standard characters. */
    os_GetStringInput("Prompt> ", out, out_size - 2);

    size_t len = strlen(out);
    if (len == 0)
        return false;

    /* Append newline required by the Python bridge's readline() call. */
    out[len]     = '\n';
    out[len + 1] = '\0';
    return true;
}

/* -----------------------------------------------
 * USB / serial event handler
 * ----------------------------------------------- */
static srl_device_t srl;
static bool         has_srl_device = false;
static uint8_t      srl_buf[512];

static usb_error_t handle_usb_event(usb_event_t event, void *event_data,
                                    usb_callback_data_t *callback_data __attribute__((unused)))
{
    usb_error_t err;
    if ((err = srl_UsbEventCallback(event, event_data, callback_data)) != USB_SUCCESS)
        return err;

    /* Reset newly connected downstream device */
    if (event == USB_DEVICE_CONNECTED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))
        usb_ResetDevice((usb_device_t)event_data);

    /* Open serial on host-configure or device-enabled events */
    if (event == USB_HOST_CONFIGURE_EVENT ||
        (event == USB_DEVICE_ENABLED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))) {

        if (has_srl_device)
            return USB_SUCCESS;

        usb_device_t device;
        if (event == USB_HOST_CONFIGURE_EVENT) {
            device = usb_FindDevice(NULL, NULL, USB_SKIP_HUBS);
            if (device == NULL)
                return USB_SUCCESS;
        } else {
            device = (usb_device_t)event_data;
        }

        srl_error_t srl_err = srl_Open(&srl, device, srl_buf, sizeof srl_buf,
                                       SRL_INTERFACE_ANY, 9600);
        if (srl_err != SRL_SUCCESS) {
            os_ClrHome();
            os_FontSelect(os_SmallFont);
            printf("Serial open failed\n"
                   "Error code: %d\n\n"
                   "Try:\n"
                   " - Unplug and replug\n"
                   "   the USB cable\n"
                   " - Restart calcgpt.py\n\n"
                   "[CLR] to quit", (int)srl_err);
            return USB_SUCCESS;
        }

        has_srl_device = true;
        app_state      = STATE_CONNECTED;
        show_status_screen();
    }

    /* Handle disconnect */
    if (event == USB_DEVICE_DISCONNECTED_EVENT) {
        usb_device_t device = (usb_device_t)event_data;
        if (device == srl.dev) {
            srl_Close(&srl);
            has_srl_device = false;
            app_state      = STATE_DISCONNECTED;
            show_status_screen();
        }
    }

    return USB_SUCCESS;
}

/* -----------------------------------------------
 * Main
 * ----------------------------------------------- */
int main(void)
{
    buf_reset();
    os_ClrHome();
    os_FontSelect(os_SmallFont);

    const usb_standard_descriptors_t *desc = srl_GetCDCStandardDescriptors();
    usb_error_t usb_error = usb_Init(handle_usb_event, NULL, desc, USB_DEFAULT_INIT_FLAGS);
    if (usb_error) {
        usb_Cleanup();
        os_ClrHome();
        printf("USB init failed\n"
               "Error code: %u\n\n"
               "Make sure no other\n"
               "USB app is running.\n\n"
               "[CLR] to quit", usb_error);
        do { kb_Scan(); } while (!kb_IsDown(kb_KeyClear));
        return 1;
    }

    app_state = STATE_DISCONNECTED;
    show_status_screen();

    /* Debounce counters */
    uint8_t scroll_debounce = 0;
    bool    second_held     = false;  /* true while [2nd] remains down */

    do {
        kb_Scan();
        usb_HandleEvents();

        if (!has_srl_device)
            continue;

        /* --- Read incoming serial data --- */
        char in_buf[65];
        memset(in_buf, 0, sizeof(in_buf));
        int bytes_read = srl_Read(&srl, in_buf, sizeof(in_buf) - 1);

        if (bytes_read < 0) {
            /* Serial read error */
            os_ClrHome();
            os_FontSelect(os_SmallFont);
            printf("Serial read error\n"
                   "Code: %d\n\n"
                   "Reconnect USB cable\n"
                   "and restart calcgpt.py\n\n"
                   "[CLR] to quit", bytes_read);
            has_srl_device = false;
            app_state      = STATE_DISCONNECTED;
        } else if (bytes_read > 0) {
            in_buf[bytes_read] = '\0';

            /* The Python bridge sends "ack" as a connection handshake.
             * Ignore it — we already switched to STATE_CONNECTED in the
             * USB event handler. */
            if (!(app_state == STATE_CONNECTED &&
                  bytes_read <= 4 && strncmp(in_buf, "ack", 3) == 0)) {
                buf_append(in_buf);
                if (app_state == STATE_WAITING_RESPONSE)
                    app_state = STATE_CONNECTED;
                /* Auto-scroll to the latest page */
                show_page(last_page >= 0 ? last_page : 0);
            }
        }

        /* --- [2nd] key: open prompt (debounced, blocked while waiting) --- */
        bool second_now = (kb_Data[1] & kb_2nd) != 0;
        if (second_now && !second_held && app_state == STATE_CONNECTED) {
            second_held = true;

            char prompt[INPUT_MAX + 2];
            if (take_input(prompt, sizeof(prompt))) {
                /* Show the user's own message in the view buffer.
                 * you_line is INPUT_MAX+8 bytes; "You: " prefix is 5 bytes,
                 * leaving enough room for the full prompt + '\n' + '\0'. */
                char you_line[INPUT_MAX + 8];
                snprintf(you_line, sizeof(you_line), "You: %s", prompt);
                buf_append(you_line);

                srl_Write(&srl, prompt, strlen(prompt));
                app_state = STATE_WAITING_RESPONSE;
                show_status_screen();
            } else {
                /* Empty input — go back to the current view */
                if (last_page >= 0)
                    show_page(last_page);
                else
                    show_status_screen();
            }
        } else if (!second_now) {
            second_held = false;
        }

        /* --- Scroll keys (with debounce for smooth, controlled movement) --- */
        if (scroll_debounce > 0) {
            scroll_debounce--;
        } else {
            int max_pg = (total_pages > 0 ? total_pages - 1 : 0);

            if (kb_Data[7] & kb_Down) {
                if (scroll_pos < max_pg) {
                    show_page(scroll_pos + 1);
                }
                scroll_debounce = 8;
            } else if (kb_Data[7] & kb_Up) {
                if (scroll_pos > 0) {
                    show_page(scroll_pos - 1);
                }
                scroll_debounce = 8;
            } else if (kb_Data[7] & kb_Right) {
                /* Jump directly to the most-recent page */
                if (last_page >= 0)
                    show_page(last_page);
                scroll_debounce = 16;
            } else if (kb_Data[7] & kb_Left) {
                /* Jump directly to the first page */
                if (total_pages > 0)
                    show_page(0);
                scroll_debounce = 16;
            }
        }

    } while (!kb_IsDown(kb_KeyClear));

    usb_Cleanup();
    return 0;
}
