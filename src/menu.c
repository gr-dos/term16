/*
 * menu.c - the configuration menu
 *
 * Up and Down choose a line, Left and Right (or Enter) change a
 * setting, Enter on the download directory edits it, Enter on "Start
 * session" applies the settings, Esc backs out.  The status bar stays
 * on the bottom row, showing the settings being edited.
 */

#include <string.h>
#include "term16.h"

#define ROW0     4
#define LABEL_C  8
#define VALUE_C  30
#define VALUE_W  (SCR_COLS - VALUE_C - 2)

enum { I_START = F_COUNT, I_EXIT, I_COUNT };

static const char *const labels[I_COUNT] = {
    "Port", "Baud rate", "Data bits", "Parity", "Stop bits",
    "Flow control", "Terminal", "Download directory", "", ""
};

static unsigned char a_norm, a_sel, a_title, a_msg, a_bar;

static int item_row(int i)
{
    return ROW0 + i + (i >= I_START ? 1 : 0);
}

static void blank(int row)
{
    scr_fill((unsigned)(row * SCR_COLS), (cell_t)(a_norm << 8 | ' '),
             SCR_COLS);
}

/* The last VALUE_W characters of s, when longer */
static const char *tail(const char *s)
{
    size_t n = strlen(s);

    return n > VALUE_W ? s + n - VALUE_W : s;
}

static void draw_item(const struct config *c, int i, int sel, int in_session)
{
    char buf[16];
    int row = item_row(i);
    unsigned char a = (unsigned char)(sel ? a_sel : a_norm);

    blank(row);
    if (i == I_START) {
        scr_text(row, LABEL_C - 2, in_session ? "  Return to session  "
                                              : "  Start session  ", a);
        return;
    }
    if (i == I_EXIT) {
        scr_text(row, LABEL_C - 2, "  Exit term16  ", a);
        return;
    }
    scr_fill((unsigned)(row * SCR_COLS + LABEL_C - 2),
             (cell_t)(a << 8 | ' '), SCR_COLS - LABEL_C);
    scr_text(row, LABEL_C, labels[i], a);
    scr_text(row, VALUE_C, i == F_DIR ? tail(c->dldir)
                                      : cfg_name(c, i, buf), a);
}

static void draw_status(const struct config *c, int state)
{
    char line[SCR_COLS + 1];

    cfg_status(c, state, line);
    scr_text(STATUS_ROW, 0, line, a_bar);
}

static void message(const char *s)
{
    blank(ROW0 + I_COUNT + 2);
    if (s != NULL)
        scr_text(ROW0 + I_COUNT + 2, LABEL_C - 2, s, a_msg);
}

/* Edit the download directory in place; 1 if changed. */
static int edit_dir(struct config *c, int state)
{
    char text[DIR_MAX], full[DIR_MAX];
    int n = (int)strlen(c->dldir), row = item_row(F_DIR), k;

    strcpy(text, c->dldir);
    message("Type the directory, Enter to accept, Esc to cancel.");
    for (;;) {
        const char *t = tail(text);
        int col = VALUE_C + (int)strlen(t);

        scr_fill((unsigned)(row * SCR_COLS + VALUE_C),
                 (cell_t)(a_sel << 8 | ' '), VALUE_W + 1);
        scr_text(row, VALUE_C, t, a_sel);
        scr_cursor(row, col > SCR_COLS - 1 ? SCR_COLS - 1 : col);
        while ((k = key_get()) == 0)
            scr_poll();
        if (k == K_ESC)
            return 0;
        if (k == '\r') {
            if (n == 0 || df_fullpath(full, text) < 0 || !df_isdir(full)) {
                message("Not an existing directory.");
                continue;
            }
            strcpy(c->dldir, full);
            draw_status(c, state);
            return 1;
        }
        if (k == '\b') {
            if (n > 0)
                text[--n] = '\0';
        } else if (k >= ' ' && k < 0x7F && n < DIR_MAX - 1) {
            text[n++] = (char)k;
            text[n] = '\0';
        }
    }
}

int menu_run(struct config *c, int in_session, int state, const char *msg)
{
    struct config work = *c;
    int sel = I_START, i, k;
    char title[SCR_COLS];

    a_norm = 0x07;
    a_sel = 0x70;
    a_title = scr_mono ? 0x0F : 0x0B;
    a_msg = scr_mono ? 0x0F : 0x0E;
    a_bar = 0x70;

    scr_fill(0, (cell_t)(a_norm << 8 | ' '), SCR_COLS * STATUS_ROW);
    strcpy(title, "term16 " VERSION " - serial terminal");
    scr_text(1, LABEL_C - 2, title, a_title);
    scr_text(ROW0 + I_COUNT + 4, LABEL_C - 2,
             "Up/Down: choose   Left/Right/Enter: change   "
             "Esc: back   Alt-X: exit", a_norm);
    scr_text(ROW0 + I_COUNT + 5, LABEL_C - 2,
             "In a session: Alt-M opens this menu, Alt-X exits.", a_norm);
    message(msg);
    scr_cursor_show(0);

    for (;;) {
        for (i = 0; i < I_COUNT; i++)
            draw_item(&work, i, i == sel, in_session);
        draw_status(&work, state);
        while ((k = key_get()) == 0)
            scr_poll();
        if (k != K_ESC && k != '\r')
            message(NULL);
        switch (k) {
        case K_UP:
            sel = (sel + I_COUNT - 1) % I_COUNT;
            break;
        case K_DOWN:
        case '\t':
            sel = (sel + 1) % I_COUNT;
            break;
        case K_HOME:
            sel = 0;
            break;
        case K_END:
            sel = I_COUNT - 1;
            break;
        case K_LEFT:
        case K_RIGHT:
            if (sel < F_DIR)
                cfg_step(&work, sel, k == K_RIGHT ? 1 : -1);
            break;
        case '\r':
            if (sel < F_DIR)
                cfg_step(&work, sel, 1);
            else if (sel == F_DIR) {
                scr_cursor_show(1);
                edit_dir(&work, state);
                scr_cursor_show(0);
                message(NULL);
            } else if (sel == I_START) {
                *c = work;
                scr_cursor_show(1);
                return MENU_START;
            } else {
                scr_cursor_show(1);
                return MENU_EXIT;
            }
            break;
        case K_ESC:
            scr_cursor_show(1);
            return in_session ? MENU_RESUME : MENU_EXIT;
        case K_ALT_X:
            scr_cursor_show(1);
            return MENU_EXIT;
        }
    }
}
