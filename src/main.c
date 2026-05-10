#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "fff.h"

typedef struct App {
    void *fff;
    char query[512];
    size_t query_len;
    uint32_t selected;
    FffSearchResult *result;
    FffGrepResult *grep_result;
    bool grep_mode;
    struct termios original_termios;
    bool raw;
    int rows;
    int cols;
} App;

static void die(const char *message) {
    fprintf(stderr, "f3: %s\n", message);
    exit(1);
}

static char *join_query(int argc, char **argv, int start) {
    size_t total = 0;
    for (int i = start; i < argc; i++) total += strlen(argv[i]) + 1;
    char *query = calloc(total + 1, 1);
    if (!query) die("out of memory");
    for (int i = start; i < argc; i++) {
        if (i > start) strcat(query, " ");
        strcat(query, argv[i]);
    }
    return query;
}

static void free_result(App *app) {
    if (app->result) {
        fff_free_search_result(app->result);
        app->result = NULL;
    }
    if (app->grep_result) {
        fff_free_grep_result(app->grep_result);
        app->grep_result = NULL;
    }
}

static void check_result(FffResult *result, const char *operation) {
    if (!result) die("fff returned null result");
    if (!result->success) {
        fprintf(stderr, "f3: %s failed: %s\n", operation, result->error ? result->error : "unknown error");
        fff_free_result(result);
        exit(1);
    }
}

static void refresh(App *app) {
    free_result(app);
    FffResult *result;
    if (app->grep_mode) {
        result = fff_live_grep(app->fff, app->query, 0, 0, 0, true, 0, 200, 100, 0, 0, false);
        check_result(result, "grep");
        app->grep_result = result->handle;
    } else {
        result = fff_search(app->fff, app->query, NULL, 0, 0, 200, 0, 0);
        check_result(result, "search");
        app->result = result->handle;
    }
    fff_free_result(result);
    uint32_t count = app->grep_mode ? (app->grep_result ? app->grep_result->count : 0) : (app->result ? app->result->count : 0);
    if (app->selected >= count) app->selected = count ? count - 1 : 0;
}

static void create_fff(App *app, bool watch) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) die("could not read current directory");
    FffResult *result = fff_create_instance2(cwd, NULL, NULL, false, true, false, watch, false, NULL, NULL, 0, 0, 0);
    check_result(result, "create instance");
    app->fff = result->handle;
    fff_free_result(result);
    result = fff_wait_for_scan(app->fff, 10000);
    check_result(result, "initial scan");
    fff_free_result(result);
}

static void update_size(App *app) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        app->rows = ws.ws_row;
        app->cols = ws.ws_col;
    } else {
        app->rows = 24;
        app->cols = 80;
    }
}

static void write_all(const char *s) {
    size_t len = strlen(s);
    while (len) {
        ssize_t written = write(STDOUT_FILENO, s, len);
        if (written <= 0) return;
        s += written;
        len -= (size_t)written;
    }
}

static void disable_raw(App *app) {
    if (app->raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &app->original_termios);
        write_all("\x1b[?1000l\x1b[?1006l\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
        app->raw = false;
    }
}

static void enable_raw(App *app) {
    if (tcgetattr(STDIN_FILENO, &app->original_termios) == -1) die("could not read terminal settings");
    struct termios raw = app->original_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("could not enable raw mode");
    app->raw = true;
}

static void draw_line(const char *text, int width) {
    int n = 0;
    char ch[2] = {0, 0};
    for (; text && text[n] && n < width; n++) {
        ch[0] = text[n];
        write_all(ch);
    }
    while (n++ < width) write_all(" ");
}

static void draw(App *app) {
    update_size(app);
    char buf[1024];
    write_all("\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[H\x1b[2K");
    snprintf(buf, sizeof(buf), "%s › %s", app->grep_mode ? "grep" : "files", app->query_len ? app->query : "");
    draw_line(buf, app->cols);
    write_all("\r\n");

    uint32_t count = app->grep_mode ? (app->grep_result ? app->grep_result->count : 0) : (app->result ? app->result->count : 0);
    int list_rows = app->rows - 3;
    uint32_t start = 0;
    if (app->selected >= (uint32_t)list_rows) start = app->selected - (uint32_t)list_rows + 1;

    for (int row = 0; row < list_rows; row++) {
        uint32_t index = start + (uint32_t)row;
        if (index < count) {
            if (index == app->selected) write_all("\x1b[7m");
            if (app->grep_mode) {
                FffGrepMatch *item = &app->grep_result->items[index];
                snprintf(buf, sizeof(buf), " %s %s:%llu:%u: %s", index == app->selected ? "›" : " ", item->relative_path, (unsigned long long)item->line_number, item->col, item->line_content);
            } else {
                FffFileItem *item = &app->result->items[index];
                snprintf(buf, sizeof(buf), " %s %s", index == app->selected ? "›" : " ", item->relative_path);
            }
            draw_line(buf, app->cols);
            write_all("\x1b[0m\r\n");
        } else {
            draw_line("", app->cols);
            write_all("\r\n");
        }
    }

    snprintf(buf, sizeof(buf), " %u results · ctrl-g %s · enter open · esc quit", count, app->grep_mode ? "files" : "grep");
    draw_line(buf, app->cols);
}

static int read_key(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == 27) {
        unsigned char seq[32];
        ssize_t len = read(STDIN_FILENO, seq, sizeof(seq) - 1);
        if (len <= 0) return 27;
        seq[len] = 0;
        if (len >= 2 && seq[0] == '[' && seq[1] == 'A') return 1000;
        if (len >= 2 && seq[0] == '[' && seq[1] == 'B') return 1001;
        if (len >= 4 && seq[0] == '[' && seq[1] == '<') {
            int button = 0;
            if (sscanf((char *)seq, "[<%d;", &button) == 1) {
                if (button == 64) return 1000;
                if (button == 65) return 1001;
            }
        }
        return 27;
    }
    return c;
}

static void open_path_at(const char *path, uint64_t line) {
    const char *editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vi";
    char line_arg[64];
    snprintf(line_arg, sizeof(line_arg), "+%llu", (unsigned long long)line);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "f3: could not open %s: %s\n", path, strerror(errno));
        return;
    }
    if (pid == 0) {
        if (line) execlp(editor, editor, line_arg, path, (char *)NULL);
        execlp(editor, editor, path, (char *)NULL);
        fprintf(stderr, "f3: could not run editor %s: %s\n", editor, strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
}

static void open_path(const char *path) {
    open_path_at(path, 0);
}

static void open_selected(App *app) {
    if (app->grep_mode) {
        if (app->grep_result && app->grep_result->count && app->selected < app->grep_result->count) {
            FffGrepMatch *item = &app->grep_result->items[app->selected];
            open_path_at(item->relative_path, item->line_number);
        }
    } else if (app->result && app->result->count && app->selected < app->result->count) {
        open_path(app->result->items[app->selected].relative_path);
    }
}

static void run_tui(App *app) {
    enable_raw(app);
    refresh(app);
    for (;;) {
        draw(app);
        int key = read_key();
        if (key < 0) continue;
        if (key == 3 || key == 27) break;
        if (key == '\r' || key == '\n') {
            disable_raw(app);
            open_selected(app);
            return;
        }
        if (key == 7) {
            app->grep_mode = !app->grep_mode;
            app->selected = 0;
            refresh(app);
            continue;
        }
        if (key == 127 || key == 8) {
            if (app->query_len) app->query[--app->query_len] = '\0';
            app->selected = 0;
            refresh(app);
            continue;
        }
        if (key == 1000) {
            if (app->selected > 0) app->selected--;
            continue;
        }
        if (key == 1001) {
            uint32_t count = app->grep_mode ? (app->grep_result ? app->grep_result->count : 0) : (app->result ? app->result->count : 0);
            if (app->selected + 1 < count) app->selected++;
            continue;
        }
        if (isprint(key) && app->query_len + 1 < sizeof(app->query)) {
            app->query[app->query_len++] = (char)key;
            app->query[app->query_len] = '\0';
            app->selected = 0;
            refresh(app);
        }
    }
    disable_raw(app);
}

static int run_cli(App *app, const char *query) {
    FffResult *result = fff_search(app->fff, query, NULL, 0, 0, 50, 0, 0);
    check_result(result, "search");
    FffSearchResult *search = result->handle;
    fff_free_result(result);
    for (uint32_t i = 0; search && i < search->count; i++) printf("%s\n", search->items[i].relative_path);
    if (search) fff_free_search_result(search);
    return 0;
}

static int run_cli_grep(App *app, const char *query) {
    FffResult *result = fff_live_grep(app->fff, query, 0, 0, 0, true, 0, 50, 0, 0, 0, false);
    check_result(result, "grep");
    FffGrepResult *grep = result->handle;
    fff_free_result(result);
    for (uint32_t i = 0; grep && i < grep->count; i++) {
        FffGrepMatch *item = &grep->items[i];
        printf("%s:%llu:%u: %s\n", item->relative_path, (unsigned long long)item->line_number, item->col, item->line_content);
    }
    if (grep) fff_free_grep_result(grep);
    return 0;
}

static bool is_f3g(const char *arg0) {
    const char *name = strrchr(arg0, '/');
    name = name ? name + 1 : arg0;
    return strcmp(name, "f3g") == 0;
}

int main(int argc, char **argv) {
    bool grep_mode = is_f3g(argv[0]);
    int query_start = 1;
    if (argc > 1 && (strcmp(argv[1], "-g") == 0 || strcmp(argv[1], "--grep") == 0)) {
        grep_mode = true;
        query_start = 2;
    }

    App app;
    memset(&app, 0, sizeof(app));
    app.grep_mode = grep_mode;
    create_fff(&app, argc == query_start);
    int exit_code = 0;
    if (argc > query_start) {
        char *query = join_query(argc, argv, query_start);
        exit_code = grep_mode ? run_cli_grep(&app, query) : run_cli(&app, query);
        free(query);
    } else {
        run_tui(&app);
    }
    free_result(&app);
    if (app.fff) fff_destroy(app.fff);
    return exit_code;
}
