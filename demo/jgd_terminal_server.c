// Minimal jgd JSONL terminal renderer for DuckTinyCC demos.
//
// This is intentionally small and dependency-free C. It implements just enough
// of the jgd protocol for the demo: accept one TCP client, respond to
// server_info/metrics requests, collect frame messages, parse line/circle/text
// operations, and render them as an ANSI-colored terminal canvas.

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define CANVAS_COLS 100
#define CANVAS_ROWS 36

typedef struct {
    unsigned char r, g, b;
    int set;
} Color;

typedef struct {
    char ch[8];
    Color color;
} Cell;

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StringVec;

static void die(const char *msg) {
    perror(msg);
    exit(2);
}

static void vec_push(StringVec *v, const char *s) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 16;
        char **items = (char **)realloc(v->items, ncap * sizeof(char *));
        if (!items) die("realloc");
        v->items = items;
        v->cap = ncap;
    }
    v->items[v->count] = strdup(s);
    if (!v->items[v->count]) die("strdup");
    v->count++;
}

static void vec_free(StringVec *v) {
    for (size_t i = 0; i < v->count; i++) free(v->items[i]);
    free(v->items);
}

static int send_all(int fd, const char *s) {
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = send(fd, s + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static const char *find_key(const char *start, const char *end, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = start;
    while (p && (!end || p < end)) {
        p = strstr(p, pat);
        if (!p || (end && p >= end)) return NULL;
        const char *q = p + strlen(pat);
        while ((!end || q < end) && isspace((unsigned char)*q)) q++;
        if ((!end || q < end) && *q == ':') return q + 1;
        p = q;
    }
    return NULL;
}

static double json_num(const char *start, const char *end, const char *key, double fallback) {
    const char *p = find_key(start, end, key);
    if (!p) return fallback;
    while ((!end || p < end) && isspace((unsigned char)*p)) p++;
    char *ep = NULL;
    double x = strtod(p, &ep);
    return ep && ep != p ? x : fallback;
}

static long json_int(const char *start, const char *end, const char *key, long fallback) {
    return (long)json_num(start, end, key, (double)fallback);
}

static bool json_str(const char *start, const char *end, const char *key, char *out, size_t outsz) {
    const char *p = find_key(start, end, key);
    if (!p || outsz == 0) return false;
    while ((!end || p < end) && isspace((unsigned char)*p)) p++;
    if ((end && p >= end) || *p != '"') return false;
    p++;
    size_t n = 0;
    while ((!end || p < end) && *p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            default: out[n++] = *p; break;
            }
        } else {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = '\0';
    return true;
}

static Color parse_rgba_value(const char *start, const char *end, const char *key, Color fallback) {
    char s[128];
    if (!json_str(start, end, key, s, sizeof(s))) return fallback;
    int r = 220, g = 220, b = 220;
    double a = 1.0;
    if (sscanf(s, "rgba(%d,%d,%d,%lf)", &r, &g, &b, &a) < 3) return fallback;
    if (a <= 0.0) return fallback;
    if (r == 0 && g == 0 && b == 0) r = g = b = 230;
    Color c = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 1};
    return c;
}

static Color op_color(const char *start, const char *end) {
    Color fallback = {230, 230, 230, 1};
    Color c = parse_rgba_value(start, end, "fill", (Color){0, 0, 0, 0});
    if (c.set) return c;
    return parse_rgba_value(start, end, "col", fallback);
}

static void clear_canvas(Cell canvas[CANVAS_ROWS][CANVAS_COLS]) {
    for (int r = 0; r < CANVAS_ROWS; r++) {
        for (int c = 0; c < CANVAS_COLS; c++) {
            strcpy(canvas[r][c].ch, " ");
            canvas[r][c].color.set = 0;
        }
    }
}

static int map_x(double x, double width) {
    int c = (int)llround(x / width * (CANVAS_COLS - 1));
    if (c < 0) c = 0;
    if (c >= CANVAS_COLS) c = CANVAS_COLS - 1;
    return c;
}

static int map_y(double y, double height) {
    int r = (int)llround(y / height * (CANVAS_ROWS - 1));
    if (r < 0) r = 0;
    if (r >= CANVAS_ROWS) r = CANVAS_ROWS - 1;
    return r;
}

static void put_cell(Cell canvas[CANVAS_ROWS][CANVAS_COLS], int c, int r, const char *ch, Color color) {
    if (c < 0 || c >= CANVAS_COLS || r < 0 || r >= CANVAS_ROWS) return;
    snprintf(canvas[r][c].ch, sizeof(canvas[r][c].ch), "%s", ch);
    canvas[r][c].color = color;
}

static void draw_line(Cell canvas[CANVAS_ROWS][CANVAS_COLS], double dev_w, double dev_h,
                      double x1, double y1, double x2, double y2, Color color) {
    int c1 = map_x(x1, dev_w), r1 = map_y(y1, dev_h);
    int c2 = map_x(x2, dev_w), r2 = map_y(y2, dev_h);
    int dc = c2 - c1, dr = r2 - r1;
    int steps = abs(dc) > abs(dr) ? abs(dc) : abs(dr);
    if (steps < 1) steps = 1;
    const char *ch = "─";
    if (abs(dc) <= (abs(dr) > 3 ? abs(dr) / 3 : 1)) ch = "│";
    else if (abs(dr) > (abs(dc) > 3 ? abs(dc) / 3 : 1)) ch = (dc * dr > 0) ? "╲" : "╱";
    for (int i = 0; i <= steps; i++) {
        int c = (int)llround(c1 + (double)dc * i / steps);
        int r = (int)llround(r1 + (double)dr * i / steps);
        put_cell(canvas, c, r, ch, color);
    }
}

static const char *next_op_bound(const char *p) {
    const char *q = strstr(p + 5, "\"op\":");
    return q ? q : p + strlen(p);
}

static void render_frames(StringVec *frames) {
    Cell canvas[CANVAS_ROWS][CANVAS_COLS];
    clear_canvas(canvas);
    double dev_w = 768.0, dev_h = 576.0;
    size_t op_count = 0, circle_count = 0, line_count = 0, text_count = 0, clip_count = 0, polygon_count = 0;

    for (size_t fi = 0; fi < frames->count; fi++) {
        const char *frame = frames->items[fi];
        const char *dev = strstr(frame, "\"device\":");
        if (dev) {
            dev_w = json_num(dev, NULL, "width", dev_w);
            dev_h = json_num(dev, NULL, "height", dev_h);
        }
        const char *p = frame;
        while ((p = strstr(p, "\"op\":"))) {
            const char *end = next_op_bound(p);
            char op[32];
            if (!json_str(p, end, "op", op, sizeof(op))) { p += 5; continue; }
            op_count++;
            Color color = op_color(p, end);
            if (strcmp(op, "line") == 0) {
                line_count++;
                draw_line(canvas, dev_w, dev_h,
                          json_num(p, end, "x1", json_num(p, end, "x0", 0)),
                          json_num(p, end, "y1", json_num(p, end, "y0", 0)),
                          json_num(p, end, "x2", json_num(p, end, "x1", 0)),
                          json_num(p, end, "y2", json_num(p, end, "y1", 0)), color);
            } else if (strcmp(op, "circle") == 0) {
                circle_count++;
                put_cell(canvas, map_x(json_num(p, end, "x", 0), dev_w), map_y(json_num(p, end, "y", 0), dev_h), "●", color);
            } else if (strcmp(op, "text") == 0) {
                text_count++;
                char text[256];
                if (json_str(p, end, "str", text, sizeof(text))) {
                    int c = map_x(json_num(p, end, "x", 0), dev_w);
                    int r = map_y(json_num(p, end, "y", 0), dev_h);
                    double hadj = json_num(p, end, "hadj", 0);
                    c -= (int)llround(hadj * (double)strlen(text));
                    for (size_t i = 0; text[i] && c + (int)i < CANVAS_COLS; i++) {
                        char one[2] = {text[i], 0};
                        if (text[i] != '\n') put_cell(canvas, c + (int)i, r, one, color);
                    }
                }
            } else if (strcmp(op, "clip") == 0) {
                clip_count++;
            } else if (strcmp(op, "polygon") == 0) {
                polygon_count++;
            }
            p = end;
        }
    }

    printf("frames:   %zu\n", frames->count);
    printf("total operations: %zu\n", op_count);
    printf("operation histogram:\n");
    printf("  circle       %zu\n", circle_count);
    printf("  text         %zu\n", text_count);
    printf("  line         %zu\n", line_count);
    printf("  clip         %zu\n", clip_count);
    printf("  polygon      %zu\n", polygon_count);
    printf("\njgd terminal render:\n");

    for (int r = 0; r < CANVAS_ROWS; r++) {
        Color cur = {0, 0, 0, 0};
        int last_nonblank = -1;
        for (int c = 0; c < CANVAS_COLS; c++) if (strcmp(canvas[r][c].ch, " ") != 0) last_nonblank = c;
        for (int c = 0; c <= last_nonblank; c++) {
            Color col = canvas[r][c].color;
            if (col.set != cur.set || col.r != cur.r || col.g != cur.g || col.b != cur.b) {
                if (cur.set) printf("\033[0m");
                if (col.set) printf("\033[38;2;%d;%d;%dm", col.r, col.g, col.b);
                cur = col;
            }
            fputs(canvas[r][c].ch, stdout);
        }
        if (cur.set) printf("\033[0m");
        putchar('\n');
    }
}

static int parse_metrics_id(const char *line) {
    return (int)json_int(line, NULL, "id", 0);
}

static int parse_str_len(const char *line) {
    char s[512];
    if (!json_str(line, NULL, "str", s, sizeof(s))) return 1;
    return (int)strlen(s);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) die("socket");
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) die("bind");
    if (listen(server_fd, 1) != 0) die("listen");
    printf("ready tcp://127.0.0.1:%d\n", port);
    fflush(stdout);

    int fd = accept(server_fd, NULL, NULL);
    if (fd < 0) die("accept");
    FILE *fp = fdopen(fd, "r");
    if (!fp) die("fdopen");

    StringVec frames = {0};
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int welcomed = 0;
    size_t messages = 0;

    while ((n = getline(&line, &cap, fp)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        messages++;
        if (!welcomed) {
            send_all(fd, "{\"type\":\"server_info\",\"serverName\":\"ducktinycc-jgd-c-terminal\",\"protocolVersion\":1,\"transport\":\"tcp\",\"serverInfo\":{\"frontend\":\"terminal\"}}\n");
            welcomed = 1;
        }
        if (strstr(line, "\"type\":\"metrics_request\"") || strstr(line, "\"type\": \"metrics_request\"")) {
            int id = parse_metrics_id(line);
            int len = parse_str_len(line);
            char response[256];
            snprintf(response, sizeof(response), "{\"type\":\"metrics_response\",\"id\":%d,\"width\":%.1f,\"ascent\":10.0,\"descent\":3.0}\n", id, 7.2 * len);
            send_all(fd, response);
        } else if (strstr(line, "\"type\":\"frame\"") || strstr(line, "\"type\": \"frame\"")) {
            vec_push(&frames, line);
        } else if (strstr(line, "\"type\":\"close\"") || strstr(line, "\"type\": \"close\"")) {
            break;
        }
    }

    free(line);
    fclose(fp);
    close(server_fd);

    printf("messages: %zu\n", messages);
    render_frames(&frames);
    vec_free(&frames);
    return 0;
}
