/*
 * TokenMeter para Windows
 * Janelinha nativa, sempre visível, que mostra o % da janela de contexto usada
 * nas chamadas à Claude API. O seu programa envia o `usage` (ou os eventos de
 * streaming) para http://127.0.0.1:47821 e a janela se atualiza na hora.
 *
 * C puro + Win32 + Winsock. Sem dependências externas.
 *
 *   MinGW:  gcc -O2 -s -municode -mwindows tokenmeter.c -o TokenMeter.exe \
 *               -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcomctl32 -lole32 -luuid
 *   MSVC:   cl /O2 tokenmeter.c /Fe:TokenMeter.exe /link /SUBSYSTEM:WINDOWS
 *
 * Endpoints (somente 127.0.0.1):
 *   POST /usage   corpo: Message inteira, só o objeto usage, ou um evento de streaming
 *   POST /event   corpo: evento de streaming (message_start / message_delta / message_stop)
 *   POST /config  {"model": "...", "context_window": 1000000}  (aceita a resposta da Models API)
 *   POST /reset   zera o contexto; /reset?all=1 zera também os totais da sessão
 *   GET  /state   estado atual em JSON
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <wctype.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define APP_NAME        L"TokenMeter"
#ifndef TM_VERSION
#define TM_VERSION      "dev"          /* a pipeline passa a versão da tag: -DTM_VERSION=\"1.3.0\" */
#endif
#define TM_W2(x)        L##x
#define TM_W(x)         TM_W2(x)
#define TM_VERSION_W    TM_W(TM_VERSION)
#define DEFAULT_PORT    47821
#define DEFAULT_WINDOW  1000000
#define MAX_BODY        (32 * 1024 * 1024)
#define HDR_CAP         (64 * 1024)
#define STALE_MS        120000ULL

#define WM_APP_UPDATE   (WM_APP + 1)
#define WM_APP_TRAY     (WM_APP + 2)
#define WM_APP_LOG      (WM_APP + 3)
#define TIMER_TICK      1
#define TIMER_REFRESH   2
#define TIMER_FIRST     3

enum {
    IDM_COMPACT = 100, IDM_TOPMOST, IDM_TOGGLE, IDM_RESET_CTX, IDM_RESET_ALL,
    IDM_WIN_200K, IDM_WIN_1M, IDM_COPY_URL, IDM_AUTOSTART, IDM_CC_SETUP, IDM_CC_COPY, IDM_REFRESH, IDM_DEBUG, IDM_SRC_MAX, IDM_SRC_CONTEXT, IDM_SRC_5H, IDM_SRC_WEEK, IDM_SRC_OFF, IDM_EXIT
};
enum { EV_NONE = 0, EV_START, EV_DELTA, EV_STOP, EV_ERROR, EV_OTHER };
enum { U_IN = 1, U_CR = 2, U_CW = 4, U_OUT = 8 };

typedef struct { long long input, cacheRead, cacheWrite, output; } Usage;
typedef struct { int has; double pct; long long resets; } Window; /* janela de limite */
typedef struct {
    char model[128];
    long long ctxWindow;
    Usage cur;
    int calls;
    long long sesIn, sesOut;
    int streaming, pending;
    ULONGLONG lastEvent;
    /* assinatura Pro/Max (vem da status line do Claude Code) */
    Window fiveH, sevenD, spendGw;
    long long subUpdated;
    /* API (vem dos cabeçalhos anthropic-ratelimit-*) */
    int apiHas;
    long long apiLimit, apiRemaining, apiReset, apiRetryUntil, apiUpdated;
    int spendCapHit;
    long long spendCapUntil;
    char plan[16];         /* "max", "pro"... (da consulta ao Claude Code) */
} State;

static State g;
static CRITICAL_SECTION g_cs;
static HINSTANCE g_inst;
static HWND g_hwnd;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread;
static int g_port = DEFAULT_PORT;
static int g_cors = 0;
static int g_compact = 0, g_topmost = 1, g_visible = 1;
static int g_dark = 1, g_themeMode = 0; /* 0 auto, 1 escuro, 2 claro */
static int g_warn = 60, g_danger = 85;
static int g_blink = 0;
static int g_useRgn = 0;
static UINT g_dpi = 96;
static HFONT g_fSmall, g_fTitle, g_fBig, g_fPill, g_fIcon;
static int g_iconFontOk = 0;          /* Segoe Fluent Icons / MDL2 disponível */
static int g_hover = 0, g_press = 0;  /* botões do cabeçalho: 1 = atualizar, 2 = debug */
static HWND g_tip;                    /* dicas dos botões */
/* definidos mais adiante */
static WCHAR g_ccStatus[160];
static int g_ccLevel;
static volatile LONG g_refreshing;
static int g_spin;
static void tips_update(void);
static WCHAR g_ini[MAX_PATH];
static NOTIFYICONDATAW g_nid;
static HICON g_trayIcon;
static int g_trayBucket = -1;
static UINT g_msgTaskbarCreated;
static POINT g_dragOff;
static int g_dragging = 0;

static COLORREF C_BG, C_FG, C_MUTED, C_TRACK, C_OK, C_WARN, C_DANGER;

/* ------------------------------------------------------------------ */
/* formatação (pt-BR)                                                  */
/* ------------------------------------------------------------------ */
static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void fmt_int(WCHAR *out, size_t cap, long long v)
{
    WCHAR d[32];
    int n = 0;
    unsigned long long x = v < 0 ? 0 : (unsigned long long)v;
    do { d[n++] = (WCHAR)(L'0' + (int)(x % 10)); x /= 10; } while (x && n < 30);
    size_t o = 0;
    for (int i = n - 1; i >= 0 && o + 2 < cap; i--) {
        out[o++] = d[i];
        if (i > 0 && i % 3 == 0) out[o++] = L'.';
    }
    out[o] = 0;
}

static void fmt_dec1(WCHAR *out, size_t cap, double x, const WCHAR *suffix)
{
    WCHAR t[48];
    _snwprintf(t, 47, L"%.1f", x);
    t[47] = 0;
    for (WCHAR *p = t; *p; p++) if (*p == L'.') *p = L',';
    size_t n = wcslen(t);
    if (n >= 2 && t[n - 2] == L',' && t[n - 1] == L'0') t[n - 2] = 0; /* "12,0" -> "12" */
    _snwprintf(out, cap - 1, L"%s%s", t, suffix);
    out[cap - 1] = 0;
}

static void fmt_compact(WCHAR *out, size_t cap, long long v)
{
    if (v < 1000) fmt_int(out, cap, v);
    else if (v < 999950) fmt_dec1(out, cap, v / 1000.0, L"k");
    else fmt_dec1(out, cap, v / 1000000.0, L"M");
}

static void fmt_pct(WCHAR *out, size_t cap, double pct)
{
    WCHAR t[48];
    _snwprintf(t, 47, L"%.1f%%", pct);
    t[47] = 0;
    for (WCHAR *p = t; *p; p++) if (*p == L'.') *p = L',';
    wcsncpy(out, t, cap - 1);
    out[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* tempo                                                               */
/* ------------------------------------------------------------------ */
static long long now_epoch(void) { return (long long)time(NULL); }

static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* 00:00 UTC do dia 1º do mês seguinte: quando o teto de gasto da API volta */
static long long next_month_utc(long long now)
{
    time_t t = (time_t)now;
    struct tm *u = gmtime(&t);
    if (!u) return now + 30LL * 86400;
    int y = u->tm_year + 1900, m = u->tm_mon + 2;
    if (m > 12) { m = 1; y++; }
    return days_from_civil(y, m, 1) * 86400;
}

/* "2h 14min", "3d 4h", "45s" */
static void fmt_left(WCHAR *out, size_t cap, long long s)
{
    if (s <= 0) _snwprintf(out, cap - 1, L"agora");
    else if (s < 60) _snwprintf(out, cap - 1, L"%llds", s);
    else if (s < 600) _snwprintf(out, cap - 1, L"%lldmin %02llds", s / 60, s % 60);
    else if (s < 3600) _snwprintf(out, cap - 1, L"%lldmin", s / 60);
    else if (s < 86400) _snwprintf(out, cap - 1, L"%lldh %02lldmin", s / 3600, (s % 3600) / 60);
    else _snwprintf(out, cap - 1, L"%lldd %lldh", s / 86400, (s % 86400) / 3600);
    out[cap - 1] = 0;
}

/* segundos desde 1970 (UTC) -> data/hora local, pelo fuso configurado no Windows */
static int epoch_to_local(long long t, SYSTEMTIME *out)
{
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
    FILETIME ft = { u.LowPart, u.HighPart };
    SYSTEMTIME st;
    return FileTimeToSystemTime(&ft, &st) && SystemTimeToTzSpecificLocalTime(NULL, &st, out);
}

/* horário local do reset: "14:30", "sáb 14:30" ou "01/11 21:00" */
static void fmt_clock(WCHAR *out, size_t cap, long long when, long long now)
{
    static const WCHAR *wd[] = { L"dom", L"seg", L"ter", L"qua", L"qui", L"sex", L"s\u00E1b" };
    SYSTEMTIME tw, tn;
    out[0] = 0;
    if (!epoch_to_local(when, &tw) || !epoch_to_local(now, &tn)) return;
    if (tw.wYear == tn.wYear && tw.wMonth == tn.wMonth && tw.wDay == tn.wDay)
        _snwprintf(out, cap - 1, L"%02d:%02d", tw.wHour, tw.wMinute);
    else if (when - now < 6 * 86400)
        _snwprintf(out, cap - 1, L"%s %02d:%02d", wd[tw.wDayOfWeek % 7], tw.wHour, tw.wMinute);
    else
        _snwprintf(out, cap - 1, L"%02d/%02d %02d:%02d", tw.wDay, tw.wMonth, tw.wHour, tw.wMinute);
    out[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* log interno (janela de debug + %APPDATA%\TokenMeter.log)            */
/* ------------------------------------------------------------------ */
#define LOG_MAX 500
enum { LV_INFO = 0, LV_WARN = 1, LV_ERR = 2 };
static WCHAR *g_log[LOG_MAX];
static int g_logHead = 0, g_logCount = 0;
static CRITICAL_SECTION g_logCs;
static int g_logReady = 0;          /* 1 na janela principal; 0 no modo --statusline (só arquivo) */
static WCHAR g_logPath[MAX_PATH];
static volatile LONG g_unseenErr = 0;
static HWND g_dbg;                  /* janela de debug (quando aberta) */

static void log_to_file(const WCHAR *line)
{
    if (!g_logPath[0]) return;
    char b[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, b, sizeof b - 2, NULL, NULL);
    if (n <= 1) return;
    b[n - 1] = '\r'; b[n] = '\n';
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, b, (DWORD)(n + 1), &w, NULL);
    CloseHandle(f);
}

static void logmsg(int level, const WCHAR *fmt, ...)
{
    WCHAR msg[1200], line[1300];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1199, fmt, ap);
    va_end(ap);
    msg[1199] = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const WCHAR *tag = level == LV_ERR ? L"ERRO " : level == LV_WARN ? L"AVISO" : L"info ";
    _snwprintf(line, 1299, L"%02d:%02d:%02d  %s  %s", t.wHour, t.wMinute, t.wSecond, tag, msg);
    line[1299] = 0;
    log_to_file(line);
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    int idx = (g_logHead + g_logCount) % LOG_MAX;
    if (g_logCount == LOG_MAX) { free(g_log[g_logHead]); g_logHead = (g_logHead + 1) % LOG_MAX; idx = (g_logHead + LOG_MAX - 1) % LOG_MAX; }
    else g_logCount++;
    g_log[idx] = _wcsdup(line);
    LeaveCriticalSection(&g_logCs);
    if (level == LV_ERR && !g_dbg) {
        InterlockedIncrement(&g_unseenErr);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0); /* ponto vermelho no botão */
    }
    if (g_dbg) PostMessageW(g_dbg, WM_APP_LOG, 0, 0);
}

/* trecho de texto UTF-8 (JSON, saída do Claude Code) no log, encurtado */
static void log_utf8(int level, const WCHAR *prefix, const char *s, int maxChars)
{
    WCHAR w[1000];
    if (!s) s = "";
    if (maxChars > 900) maxChars = 900;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 999);
    if (n <= 0) { wcscpy(w, L"(texto ilegível)"); n = (int)wcslen(w) + 1; }
    w[999] = 0;
    for (WCHAR *p = w; *p; p++) if (*p == L'\r' || *p == L'\n' || *p == L'\t') *p = L' ';
    if ((int)wcslen(w) > maxChars) wcscpy(w + maxChars, L"…");
    logmsg(level, L"%s%s", prefix, w);
}

static void log_clear(void)
{
    EnterCriticalSection(&g_logCs);
    for (int i = 0; i < g_logCount; i++) free(g_log[(g_logHead + i) % LOG_MAX]);
    g_logHead = g_logCount = 0;
    LeaveCriticalSection(&g_logCs);
}

static int memmem_simple(const char *h, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(h + i, needle, k)) return 1;
    return 0;
}

static void last_error_text(WCHAR *out, size_t cap, DWORD err)
{
    out[0] = 0;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, out, (DWORD)cap, NULL);
    size_t n = wcslen(out);
    while (n && (out[n - 1] == L'\r' || out[n - 1] == L'\n' || out[n - 1] == L'.')) out[--n] = 0;
    if (!out[0]) _snwprintf(out, cap - 1, L"código %lu", err);
}

/* ------------------------------------------------------------------ */
/* leitura mínima de JSON (só o que precisamos)                        */
/* ------------------------------------------------------------------ */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Acha "key": e devolve o ponteiro para o valor. Ignora aspas escapadas. */
static const char *find_key(const char *s, const char *key)
{
    if (!s) return NULL;
    size_t kl = strlen(key);
    const char *p = s;
    while ((p = strchr(p, '"')) != NULL) {
        if (p > s && p[-1] == '\\') { p++; continue; }
        if (strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
            const char *q = skip_ws(p + 2 + kl);
            if (*q == ':') return skip_ws(q + 1);
        }
        p++;
    }
    return NULL;
}

static int parse_ll(const char *v, long long *out)
{
    if (!v) return 0;
    if (*v == '-' || (*v >= '0' && *v <= '9')) {
        long long x = strtoll(v, NULL, 10);
        *out = x < 0 ? 0 : x;
        return 1;
    }
    return 0; /* null ou outro tipo */
}

static int read_str(const char *v, char *out, size_t cap)
{
    if (!v || *v != '"') return 0;
    v++;
    size_t n = 0;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) v++;
        if (n + 1 < cap) out[n++] = *v;
        v++;
    }
    out[n] = 0;
    return 1;
}

static void set_model_from(State *s, const char *v)
{
    char tmp[128];
    if (!read_str(v, tmp, sizeof tmp)) return;
    char *o = s->model;
    for (const char *p = tmp; *p && o < s->model + sizeof(s->model) - 1; p++)
        if (isalnum((unsigned char)*p) || strchr("._-:/@ ()", *p)) *o++ = *p;
    *o = 0;
}

/* Fim (exclusivo) de um objeto/array JSON que começa em p, respeitando strings. */
static const char *value_end(const char *p)
{
    if (!p || (*p != '{' && *p != '[')) return NULL;
    int depth = 0, inStr = 0;
    for (; *p; p++) {
        if (inStr) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') inStr = 0;
        } else if (*p == '"') inStr = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    }
    return NULL;
}

/* Copia o objeto que começa em v para buf, para limitar a busca de chaves a ele. */
static int copy_obj(const char *v, char *buf, size_t cap)
{
    const char *e = value_end(v);
    if (!e) return 0;
    size_t n = (size_t)(e - v);
    if (n >= cap) n = cap - 1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return 1;
}

/* número, com ou sem aspas ("2000000" nos cabeçalhos) */
static int parse_num(const char *v, double *out)
{
    if (!v) return 0;
    if (*v == '"') v++;
    if (*v == '-' || *v == '.' || (*v >= '0' && *v <= '9')) { *out = strtod(v, NULL); return 1; }
    return 0;
}

/* "2026-10-31T12:34:56Z" / "...56.123+00:00" -> segundos desde 1970 (UTC) */
static long long parse_rfc3339(const char *v)
{
    if (!v) return 0;
    if (*v == '"') v++;
    int Y, M, D, h, m, s, n = 0;
    if (sscanf(v, "%4d-%2d-%2d%*1[Tt ]%2d:%2d:%2d%n", &Y, &M, &D, &h, &m, &s, &n) < 6 || n == 0) return 0;
    const char *p = v + n;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    long long off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%2d:%2d", &oh, &om);
        off = (oh * 3600LL + om * 60LL) * (*p == '-' ? -1 : 1);
    }
    return days_from_civil(Y, M, D) * 86400 + h * 3600LL + m * 60LL + s - off;
}

static int read_window(const char *scope, const char *name, Window *w)
{
    char buf[512];
    const char *v = find_key(scope, name);
    double pct = 0, rs = 0;
    if (!v || *v != '{' || !copy_obj(v, buf, sizeof buf)) return 0;
    if (!parse_num(find_key(buf, "used_percentage"), &pct) &&
        !parse_num(find_key(buf, "utilization"), &pct)) return 0;
    const char *ra = find_key(buf, "resets_at");
    if (ra && *ra == '"') w->resets = parse_rfc3339(ra);       /* "2026-10-31T14:00:00Z" */
    else if (parse_num(ra, &rs)) w->resets = (long long)rs;   /* segundos desde 1970 */
    else w->resets = 0;                                      /* null: janela ainda não começou */
    w->pct = pct;
    w->has = 1;
    return 1;
}

static int parse_usage(const char *body, Usage *u)
{
    const char *scope = find_key(body, "usage");
    if (!scope) scope = body;
    int m = 0;
    long long v;
    if (parse_ll(find_key(scope, "input_tokens"), &v)) { u->input = v; m |= U_IN; }
    if (parse_ll(find_key(scope, "cache_read_input_tokens"), &v)) { u->cacheRead = v; m |= U_CR; }
    if (parse_ll(find_key(scope, "cache_creation_input_tokens"), &v)) { u->cacheWrite = v; m |= U_CW; }
    if (parse_ll(find_key(scope, "output_tokens"), &v)) { u->output = v; m |= U_OUT; }
    return m;
}

static int event_kind(const char *body)
{
    const char *p = body;
    int other = EV_NONE;
    while ((p = find_key(p, "type")) != NULL) {
        char t[48];
        if (read_str(p, t, sizeof t)) {
            if (!strcmp(t, "message_start")) return EV_START;
            if (!strcmp(t, "message_delta")) return EV_DELTA;
            if (!strcmp(t, "message_stop")) return EV_STOP;
            if (!strcmp(t, "error")) return EV_ERROR;
            if (!strncmp(t, "content_block_", 14) || !strcmp(t, "ping")) other = EV_OTHER;
        }
        p++;
    }
    return other;
}

/* ------------------------------------------------------------------ */
/* estado                                                              */
/* ------------------------------------------------------------------ */
static void commit_pending(void) /* chamar com g_cs travado */
{
    if (!g.pending) return;
    g.calls++;
    g.sesIn += g.cur.input + g.cur.cacheRead + g.cur.cacheWrite;
    g.sesOut += g.cur.output;
    g.pending = 0;
}

static int apply_body(const char *body, int eventOnly)
{
    int kind = event_kind(body);
    int code = 200;
    Usage u = {0};
    int m;
    EnterCriticalSection(&g_cs);
    if (kind != EV_NONE) {
        switch (kind) {
        case EV_START:
            commit_pending();
            parse_usage(body, &u);
            g.cur = u;
            set_model_from(&g, find_key(body, "model"));
            g.streaming = 1;
            g.pending = 1;
            g.lastEvent = GetTickCount64();
            break;
        case EV_DELTA: /* valores cumulativos: substituem os anteriores */
            m = parse_usage(body, &u);
            if (m & U_IN) g.cur.input = u.input;
            if (m & U_CR) g.cur.cacheRead = u.cacheRead;
            if (m & U_CW) g.cur.cacheWrite = u.cacheWrite;
            if (m & U_OUT) g.cur.output = u.output;
            g.lastEvent = GetTickCount64();
            break;
        case EV_STOP:
        case EV_ERROR:
            g.streaming = 0;
            commit_pending();
            break;
        default:
            if (g.streaming) g.lastEvent = GetTickCount64();
            break;
        }
    } else if (eventOnly) {
        code = 400;
    } else {
        m = parse_usage(body, &u);
        if (!m) code = 400;
        else {
            commit_pending();
            g.cur = u;
            set_model_from(&g, find_key(body, "model"));
            g.streaming = 0;
            g.pending = 1;
            commit_pending();
        }
    }
    LeaveCriticalSection(&g_cs);
    return code;
}

static int apply_config(const char *body)
{
    long long v;
    int changed = 0;
    EnterCriticalSection(&g_cs);
    const char *mv = find_key(body, "model");
    if (!mv && find_key(body, "max_input_tokens")) mv = find_key(body, "id"); /* resposta da Models API */
    if (mv) { set_model_from(&g, mv); changed = 1; }
    if (parse_ll(find_key(body, "context_window"), &v) && v > 0) { g.ctxWindow = v; changed = 1; }
    else if (parse_ll(find_key(body, "max_input_tokens"), &v) && v > 0) { g.ctxWindow = v; changed = 1; }
    LeaveCriticalSection(&g_cs);
    return changed ? 200 : 400;
}

/*
 * Limites. Aceita, no mesmo corpo ou separados:
 *  - o JSON que o Claude Code entrega à status line (rate_limits, context_window, model)
 *  - os cabeçalhos de resposta da API como objeto JSON ({"anthropic-ratelimit-tokens-limit": "...", ...})
 *  - o corpo de um erro 429 de teto de gasto (enforced_spend_limit_reached)
 * Não trava g_cs: quem chama decide.
 */
static int apply_limits_to(State *s, const char *body)
{
    int hit = 0;
    long long now = now_epoch();
    char big[8192];
    double d;

    /* ---- Claude Code (assinatura Pro/Max) ---- */
    const char *rl = find_key(body, "rate_limits");
    if (rl && *rl == '{' && copy_obj(rl, big, sizeof big)) {
        hit |= read_window(big, "five_hour", &s->fiveH);
        hit |= read_window(big, "seven_day", &s->sevenD);
        hit |= read_window(big, "spend_limit", &s->spendGw);
        s->subUpdated = now;
        hit = 1;
    }
    const char *cw = find_key(body, "context_window");
    if (cw && *cw == '{' && copy_obj(cw, big, sizeof big)) {
        if (parse_num(find_key(big, "context_window_size"), &d) && d > 0) s->ctxWindow = (long long)d;
        const char *cu = find_key(big, "current_usage");
        if (cu && *cu == '{') {
            Usage u = {0};
            char cubuf[1024];
            if (copy_obj(cu, cubuf, sizeof cubuf) && parse_usage(cubuf, &u)) {
                s->cur = u;           /* a status line roda várias vezes: não soma na sessão */
                s->pending = 0;
                s->streaming = 0;
            }
        }
        const char *mv = find_key(body, "model");
        if (mv && *mv == '{') {
            char mb[512];
            if (copy_obj(mv, mb, sizeof mb)) {
                const char *dn = find_key(mb, "display_name");
                set_model_from(s, dn ? dn : find_key(mb, "id"));
            }
        } else if (mv) {
            set_model_from(s, mv);
        }
        hit = 1;
    }

    /* ---- API: cabeçalhos de rate limit ---- */
    const char *pre = find_key(body, "anthropic-ratelimit-tokens-limit") ? "anthropic-ratelimit-tokens-"
                    : find_key(body, "anthropic-ratelimit-input-tokens-limit") ? "anthropic-ratelimit-input-tokens-"
                    : NULL;
    if (pre) {
        char k[96];
        snprintf(k, sizeof k, "%slimit", pre);
        if (parse_num(find_key(body, k), &d)) s->apiLimit = (long long)d;
        snprintf(k, sizeof k, "%sremaining", pre);
        if (parse_num(find_key(body, k), &d)) s->apiRemaining = (long long)d;
        snprintf(k, sizeof k, "%sreset", pre);
        s->apiReset = parse_rfc3339(find_key(body, k));
        s->apiHas = 1;
        s->apiUpdated = now;
        if (!find_key(body, "retry-after")) s->apiRetryUntil = 0;
        hit = 1;
    }
    if (parse_num(find_key(body, "retry-after"), &d)) {
        s->apiRetryUntil = now + (long long)d;
        s->apiHas = 1;
        s->apiUpdated = now;
        hit = 1;
    }
    if (strstr(body, "enforced_spend_limit_reached")) {
        s->spendCapHit = 1;
        s->spendCapUntil = next_month_utc(now);
        s->apiHas = 1;
        s->apiUpdated = now;
        hit = 1;
    }
    return hit ? 200 : 400;
}

static int apply_limits(const char *body)
{
    EnterCriticalSection(&g_cs);
    int r = apply_limits_to(&g, body);
    LeaveCriticalSection(&g_cs);
    return r;
}

static void reset_state(int all)
{
    EnterCriticalSection(&g_cs);
    memset(&g.cur, 0, sizeof g.cur);
    g.streaming = 0;
    g.pending = 0;
    if (all) { g.calls = 0; g.sesIn = g.sesOut = 0; }
    LeaveCriticalSection(&g_cs);
}

static State snapshot(void)
{
    State s;
    EnterCriticalSection(&g_cs);
    s = g;
    LeaveCriticalSection(&g_cs);
    return s;
}

static long long used_of(const State *s)
{
    return s->cur.input + s->cur.cacheRead + s->cur.cacheWrite + s->cur.output;
}

static void state_json(char *out, size_t cap)
{
    State s = snapshot();
    long long used = used_of(&s);
    double pct = s.ctxWindow > 0 ? used * 100.0 / (double)s.ctxWindow : 0.0;
    long long rem = s.ctxWindow - used;
    snprintf(out, cap,
        "{\"model\":\"%s\",\"context_window\":%lld,\"used\":%lld,\"remaining\":%lld,"
        "\"percent\":%.2f,\"streaming\":%s,"
        "\"current\":{\"input_tokens\":%lld,\"cache_read_input_tokens\":%lld,"
        "\"cache_creation_input_tokens\":%lld,\"output_tokens\":%lld},"
        "\"session\":{\"calls\":%d,\"input_tokens\":%lld,\"output_tokens\":%lld},",
        s.model, s.ctxWindow, used, rem < 0 ? 0 : rem, pct, s.streaming ? "true" : "false",
        s.cur.input, s.cur.cacheRead, s.cur.cacheWrite, s.cur.output,
        s.calls, s.sesIn, s.sesOut);
    size_t n = strlen(out);
    long long now = now_epoch();
    const Window *ws[] = { &s.fiveH, &s.sevenD, &s.spendGw };
    const char *names[] = { "five_hour", "seven_day", "spend_limit" };
    n += snprintf(out + n, cap - n, "\"limits\":{");
    for (int i = 0; i < 3; i++) {
        if (ws[i]->has)
            n += snprintf(out + n, cap - n, "\"%s\":{\"used_percentage\":%.1f,\"resets_at\":%lld,\"seconds_left\":%lld},",
                          names[i], ws[i]->pct, ws[i]->resets, ws[i]->resets > now ? ws[i]->resets - now : 0);
    }
    if (s.apiHas)
        n += snprintf(out + n, cap - n,
                      "\"api\":{\"tokens_limit\":%lld,\"tokens_remaining\":%lld,\"reset_at\":%lld,"
                      "\"retry_after_seconds\":%lld,\"spend_cap_reached\":%s,\"spend_cap_resets_at\":%lld},",
                      s.apiLimit, s.apiRemaining, s.apiReset,
                      s.apiRetryUntil > now ? s.apiRetryUntil - now : 0,
                      s.spendCapHit ? "true" : "false", next_month_utc(now));
    if (n < cap && out[n - 1] == ',') n--;
    snprintf(out + n, cap - n, "}}\n");
}

/* ------------------------------------------------------------------ */
/* servidor HTTP local                                                 */
/* ------------------------------------------------------------------ */
static int send_all(SOCKET s, const char *b, int n)
{
    while (n > 0) {
        int r = send(s, b, n, 0);
        if (r <= 0) return 0;
        b += r;
        n -= r;
    }
    return 1;
}

static void respond(SOCKET c, int code, const char *body)
{
    const char *reason =
        code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" :
        code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" :
        code == 431 ? "Request Header Fields Too Large" : "Error";
    int bl = body ? (int)strlen(body) : 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\nConnection: close\r\n%s\r\n",
        code, reason, bl,
        g_cors ? "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n" : "");
    if (n > 0 && send_all(c, hdr, n) && bl) send_all(c, body, bl);
}

static void handle_client(SOCKET c)
{
    DWORD to = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    size_t cap = HDR_CAP, len = 0;
    char *buf = (char *)malloc(cap + 1);
    char *hend = NULL;
    if (!buf) return;
    buf[0] = 0;

    while (!hend) {
        if (len >= cap) { respond(c, 431, "{\"error\":\"cabecalho grande demais\"}"); goto done; }
        int r = recv(c, buf + len, (int)(cap - len), 0);
        if (r <= 0) goto done;
        len += (size_t)r;
        buf[len] = 0;
        hend = strstr(buf, "\r\n\r\n");
    }

    size_t hlen = (size_t)(hend - buf) + 4;
    char method[8] = {0}, path[256] = {0}, query[256] = {0};
    if (sscanf(buf, "%7s %255s", method, path) != 2) goto done;
    char *qm = strchr(path, '?');
    if (qm) { strncpy(query, qm + 1, sizeof query - 1); *qm = 0; }

    long long cl = 0;
    int hasOrigin = 0;
    for (char *line = buf; line < hend;) {
        char *nl = strstr(line, "\r\n");
        if (!nl) break;
        if (_strnicmp(line, "Content-Length:", 15) == 0) cl = _atoi64(line + 15);
        else if (_strnicmp(line, "Origin:", 7) == 0) hasOrigin = 1;
        line = nl + 2;
    }
    /* Pedidos vindos de páginas web (com Origin) só são aceitos com --cors,
       para que um site qualquer aberto no navegador não mexa no medidor. */
    if (hasOrigin && !g_cors) {
        logmsg(LV_WARN, L"%S %S recusado: veio de uma p\u00E1gina web (ligue cors=1 no .ini para aceitar)", method, path);
        respond(c, 403, "{\"error\":\"pedidos de navegador exigem --cors\"}");
        goto done;
    }
    if (cl < 0 || cl > MAX_BODY) {
        logmsg(LV_WARN, L"%S %S recusado: corpo grande demais (%lld bytes)", method, path, cl);
        respond(c, 413, "{\"error\":\"corpo grande demais\"}");
        goto done;
    }

    size_t need = hlen + (size_t)cl;
    if (need > cap) {
        char *nb = (char *)realloc(buf, need + 1);
        if (!nb) goto done;
        buf = nb;
        cap = need;
    }
    while (len < need) {
        int r = recv(c, buf + len, (int)(need - len), 0);
        if (r <= 0) goto done;
        len += (size_t)r;
    }
    buf[need] = 0;
    const char *body = buf + hlen;

    char out[4096];
    if (!strcmp(method, "OPTIONS")) {
        respond(c, 204, NULL);
    } else if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/state"))) {
        state_json(out, sizeof out);
        respond(c, 200, out);
    } else if (!strcmp(method, "POST")) {
        int code = 404;
        if (!strcmp(path, "/usage")) code = apply_body(body, 0);
        else if (!strcmp(path, "/event")) code = apply_body(body, 1);
        else if (!strcmp(path, "/config")) code = apply_config(body);
        else if (!strcmp(path, "/limits")) code = apply_limits(body);
        else if (!strcmp(path, "/reset")) {
            const char *a = find_key(body, "all");
            int all = strstr(query, "all=1") != NULL || (a && !strncmp(a, "true", 4));
            reset_state(all);
            code = 200;
        }
        /* no log: tudo, menos o vaivém de message_delta do streaming */
        if (code != 200)
            log_utf8(LV_WARN, code == 404 ? L"POST em rota desconhecida: " : L"POST sem dados reconhecidos, corpo: ",
                     code == 404 ? path : body, 300);
        else if (strcmp(path, "/event") || !strstr(body, "message_delta"))
            logmsg(LV_INFO, L"POST %S recebido (%lld bytes)", path, cl);
        if (code == 200) {
            state_json(out, sizeof out);
            respond(c, 200, out);
            PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);
        } else if (code == 404) {
            respond(c, 404, "{\"error\":\"rota desconhecida: use /usage, /event, /limits, /config, /reset ou /state\"}");
        } else {
            respond(c, 400, "{\"error\":\"corpo sem dados reconhecidos (usage, evento de streaming ou limites)\"}");
        }
    } else {
        respond(c, 404, "{\"error\":\"metodo nao suportado\"}");
    }
done:
    free(buf);
}

static DWORD WINAPI server_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        handle_client(c);
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
    return 0;
}

static int start_server(void)
{
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return 0;
    BOOL excl = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* só aceita conexões desta máquina */
    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR ||
        listen(g_listen, 16) == SOCKET_ERROR) {
        WCHAR e[256];
        last_error_text(e, 256, (DWORD)WSAGetLastError());
        logmsg(LV_ERR, L"N\u00E3o consegui abrir a porta %d: %s", g_port, e);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* configurações (%APPDATA%\TokenMeter.ini) e sistema                  */
/* ------------------------------------------------------------------ */
static int ini_int(const WCHAR *k, int def)
{
    WCHAR b[32];
    GetPrivateProfileStringW(APP_NAME, k, L"", b, 32, g_ini);
    return b[0] ? _wtoi(b) : def; /* aceita negativos (monitores à esquerda) */
}
static void ini_set(const WCHAR *k, int v)
{
    WCHAR b[32];
    _snwprintf(b, 31, L"%d", v);
    b[31] = 0;
    WritePrivateProfileStringW(APP_NAME, k, b, g_ini);
}

static void read_theme(void)
{
    if (g_themeMode == 1) { g_dark = 1; return; }
    if (g_themeMode == 2) { g_dark = 0; return; }
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        g_dark = (v == 0);
    else
        g_dark = 1;
}

static void set_palette(void)
{
    if (g_dark) {
        C_BG = RGB(0x1F, 0x1F, 0x22); C_FG = RGB(0xF2, 0xF2, 0xF4); C_MUTED = RGB(0x9A, 0x9A, 0xA2);
        C_TRACK = RGB(0x3A, 0x3A, 0x40); C_OK = RGB(0x4C, 0xC3, 0x8A); C_WARN = RGB(0xE8, 0xB0, 0x4A);
        C_DANGER = RGB(0xF0, 0x6A, 0x6A);
    } else {
        C_BG = RGB(0xFF, 0xFF, 0xFF); C_FG = RGB(0x1D, 0x1D, 0x1F); C_MUTED = RGB(0x6B, 0x6B, 0x70);
        C_TRACK = RGB(0xE8, 0xE8, 0xEB); C_OK = RGB(0x2F, 0x9E, 0x6B); C_WARN = RGB(0xC9, 0x8A, 0x0B);
        C_DANGER = RGB(0xD6, 0x45, 0x45);
    }
}

static COLORREF status_color(double ratio)
{
    double p = ratio * 100.0;
    return p >= g_danger ? C_DANGER : p >= g_warn ? C_WARN : C_OK;
}

static const WCHAR *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static int autostart_get(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}
static void autostart_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR path[MAX_PATH], q[MAX_PATH + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        _snwprintf(q, MAX_PATH + 3, L"\"%s\"", path);
        q[MAX_PATH + 3] = 0;
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)q, (DWORD)((wcslen(q) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

static void copy_text(const WCHAR *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return;
    memcpy(GlobalLock(h), s, n);
    GlobalUnlock(h);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

typedef BOOL(WINAPI *PFN_SetDpiCtx)(HANDLE);
typedef UINT(WINAPI *PFN_GetDpiForWindow)(HWND);

static void set_dpi_awareness(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SetDpiCtx f = u ? (PFN_SetDpiCtx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (f && f((HANDLE)(INT_PTR)-4)) return; /* PER_MONITOR_AWARE_V2 */
    SetProcessDPIAware();
}

static UINT window_dpi(HWND h)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_GetDpiForWindow f = u ? (PFN_GetDpiForWindow)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (f) { UINT d = f(h); if (d) return d; }
    HDC dc = GetDC(h);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(h, dc);
    return d ? d : 96;
}

/* ------------------------------------------------------------------ */
/* janela                                                              */
/* ------------------------------------------------------------------ */
static HFONT mkfont(int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void make_fonts(void)
{
    HFONT *all[] = { &g_fSmall, &g_fTitle, &g_fBig, &g_fPill, &g_fIcon };
    for (int i = 0; i < 5; i++) if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
    g_fSmall = mkfont(12, FW_NORMAL);
    g_fTitle = mkfont(11, FW_SEMIBOLD);
    g_fBig = mkfont(26, FW_BOLD);
    g_fPill = mkfont(13, FW_BOLD);
    /* ícones nativos do Windows 10/11; se não houver, os botões são desenhados à mão */
    static const WCHAR *faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    g_iconFontOk = 0;
    HDC dc = GetDC(NULL);
    for (int i = 0; i < 2 && !g_iconFontOk; i++) {
        HFONT f = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, faces[i]);
        WCHAR got[LF_FACESIZE] = L"";
        HGDIOBJ o = SelectObject(dc, f);
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        if (!_wcsicmp(got, faces[i])) { g_fIcon = f; g_iconFontOk = 1; }
        else DeleteObject(f);
    }
    ReleaseDC(NULL, dc);
}

/* ---- linhas de limite (assinatura e API) ---- */
typedef struct {
    WCHAR label[16];
    double pct;        /* < 0: sem porcentagem */
    COLORREF col;
    WCHAR left[32];    /* "2h 14min" */
    WCHAR clock[32];   /* "14:30" */
} Row;

static int build_rows(const State *s, Row *rows, long long now)
{
    int n = 0;
    const Window *ws[] = { &s->fiveH, &s->sevenD, &s->spendGw };
    const WCHAR *labels[] = { L"5 horas", L"Semana", L"Gasto" };
    for (int i = 0; i < 3; i++) {
        const Window *w = ws[i];
        if (!w->has) continue;
        Row *r = &rows[n++];
        wcscpy(r->label, labels[i]);
        r->clock[0] = 0;
        if (w->resets > now) {
            r->pct = w->pct;
            fmt_left(r->left, 32, w->resets - now);
            fmt_clock(r->clock, 32, w->resets, now);
        } else {
            r->pct = 0;   /* a janela já virou: o uso recomeça do zero */
            wcscpy(r->left, L"liberado");
        }
        r->col = status_color(r->pct / 100.0);
    }
    if (s->apiHas) {
        Row *r = &rows[n++];
        wcscpy(r->label, L"API/min");
        r->clock[0] = 0;
        if (s->apiRetryUntil > now) {
            r->pct = 100;
            r->col = C_DANGER;
            WCHAR t[32];
            fmt_left(t, 32, s->apiRetryUntil - now);
            _snwprintf(r->left, 31, L"espere %s", t);
            r->left[31] = 0;
        } else if (s->apiLimit > 0) {
            /* o balde de tokens já encheu? */
            int full = s->apiRemaining >= s->apiLimit ||
                       (s->apiReset && now >= s->apiReset) ||
                       (!s->apiReset && s->apiRetryUntil && now >= s->apiRetryUntil);
            r->pct = full ? 0 : (s->apiLimit - s->apiRemaining) * 100.0 / (double)s->apiLimit;
            if (r->pct < 0) r->pct = 0;
            r->col = status_color(r->pct / 100.0);
            if (full) wcscpy(r->left, L"cheio");
            else if (s->apiReset) fmt_left(r->left, 32, s->apiReset - now);
            else wcscpy(r->left, L"\u2014");
        } else {
            r->pct = -1;
            r->col = C_MUTED;
            wcscpy(r->left, L"—");
        }

        r = &rows[n++];
        wcscpy(r->label, L"Mês API");
        long long nm = next_month_utc(now);
        int capHit = s->spendCapHit && now < s->spendCapUntil;
        r->pct = capHit ? 100 : -1;
        r->col = capHit ? C_DANGER : C_MUTED;
        fmt_left(r->left, 32, nm - now);
        fmt_clock(r->clock, 32, nm, now);
    }
    return n;
}

static int show_session_line(const State *s) { return s->calls > 0; }

static void compact_text(const State *s, WCHAR *out, size_t cap)
{
    long long used = used_of(s), now = now_epoch();
    double ratio = s->ctxWindow > 0 ? (double)used / (double)s->ctxWindow : 0.0;
    WCHAR pct[32], left[32];
    fmt_pct(pct, 32, ratio * 100.0);
    if (s->fiveH.has && s->fiveH.resets > now) {
        fmt_left(left, 32, s->fiveH.resets - now);
        _snwprintf(out, cap - 1, L"%s   5h: %s", pct, left);
    } else {
        _snwprintf(out, cap - 1, L"%s", pct);
    }
    out[cap - 1] = 0;
}

#define FULL_W 300
static void layout_size(int *w, int *h)
{
    State s = snapshot();
    if (g_compact) {
        WCHAR t[96];
        SIZE z = { 0, 0 };
        compact_text(&s, t, 96);
        HDC dc = GetDC(g_hwnd);
        HGDIOBJ o = SelectObject(dc, g_fPill);
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &z);
        SelectObject(dc, o);
        ReleaseDC(g_hwnd, dc);
        *w = S(24) + z.cx + S(14);
        *h = S(30);
        return;
    }
    Row rows[8];
    int n = build_rows(&s, rows, now_epoch());
    int hh = 98;
    if (show_session_line(&s)) hh += 18;
    if (n) hh += 8 + 18 + 20 * n + 4;
    else hh += 8 + 18 + 2;   /* linha de status dos limites */
    *w = S(FULL_W);
    *h = S(hh);
}

static void apply_corners(void)
{
    DWORD pref = 2; /* DWMWCP_ROUND */
    if (!g_useRgn && SUCCEEDED(DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof pref)))
        return;
    g_useRgn = 1; /* Windows 10: arredonda com região */
    RECT r;
    GetWindowRect(g_hwnd, &r);
    int rad = g_compact ? (r.bottom - r.top) : S(16);
    SetWindowRgn(g_hwnd, CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, rad, rad), TRUE);
}

static void clamp_to_monitor(void)
{
    RECT r;
    GetWindowRect(g_hwnd, &r);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = r.left, y = r.top;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (x != r.left || y != r.top)
        SetWindowPos(g_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void save_pos(void)
{
    RECT r;
    GetWindowRect(g_hwnd, &r);
    ini_set(L"x", r.left);
    ini_set(L"y", r.top);
}

static void resize_to_layout(void)
{
    int w, h;
    RECT r;
    layout_size(&w, &h);
    GetWindowRect(g_hwnd, &r);
    /* na metade de baixo da tela, cresce para cima (mantém a borda de baixo no lugar) */
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    int y = r.top;
    if ((r.top + r.bottom) / 2 > (mi.rcWork.top + mi.rcWork.bottom) / 2) y = r.bottom - h;
    SetWindowPos(g_hwnd, NULL, r.left, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    apply_corners();
    clamp_to_monitor();
    tips_update();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* redimensiona só se o conteúdo mudou de tamanho (linhas novas, texto do modo compacto) */
static void fit_if_needed(void)
{
    int w, h;
    RECT r;
    layout_size(&w, &h);
    GetWindowRect(g_hwnd, &r);
    if (w != r.right - r.left || h != r.bottom - r.top) resize_to_layout();
}

static void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void draw_line(HDC dc, HFONT f, COLORREF c, int l, int t, int r, int b, const WCHAR *s, UINT extra)
{
    RECT rc = { l, t, r, b };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | extra);
}

/* ---- botões do cabeçalho: ↻ atualizar e 🐞 debug ---- */
static void header_buttons(int w, RECT *rr, RECT *rd)
{
    int sz = S(22), top = S(4), right = w - S(6);
    SetRect(rd, right - sz, top, right, top + sz);
    SetRect(rr, rd->left - S(2) - sz, top, rd->left - S(2), top + sz);
}

static int hit_button(int x, int y)
{
    if (g_compact || !g_hwnd) return 0;
    RECT rc, rr, rd;
    POINT p = { x, y };
    GetClientRect(g_hwnd, &rc);
    header_buttons(rc.right, &rr, &rd);
    if (PtInRect(&rr, p)) return 1;
    if (PtInRect(&rd, p)) return 2;
    return 0;
}

static void rotate_dc(HDC dc, double deg, int cx, int cy)
{
    double a = deg * 3.14159265358979 / 180.0, c = cos(a), s = sin(a);
    XFORM x;
    x.eM11 = (FLOAT)c; x.eM12 = (FLOAT)s; x.eM21 = (FLOAT)-s; x.eM22 = (FLOAT)c;
    x.eDx = (FLOAT)(cx - cx * c + cy * s);
    x.eDy = (FLOAT)(cy - cx * s - cy * c);
    SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &x);
}

static void reset_dc(HDC dc)
{
    ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
    SetGraphicsMode(dc, GM_COMPATIBLE);
}

static void draw_header_button(HDC dc, const RECT *r, int which)
{
    int hover = g_hover == which, pressed = g_press == which && hover;
    int busy = which == 1 && g_refreshing;
    if (hover || pressed) fill_round(dc, r->left, r->top, r->right, r->bottom, S(6), pressed ? C_MUTED : C_TRACK);
    COLORREF col = busy ? C_OK : hover ? C_FG : C_MUTED;
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;

    if (busy) rotate_dc(dc, g_spin, cx, cy);
    if (g_iconFontOk && g_fIcon) {
        WCHAR glyph[2] = { which == 1 ? 0xE72C /* Refresh */ : 0xEBE8 /* Bug */, 0 };
        RECT t = *r;
        SelectObject(dc, g_fIcon);
        SetTextColor(dc, col);
        DrawTextW(dc, glyph, 1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        int pw = g_dpi >= 144 ? S(1) + 1 : 2;
        HPEN pen = CreatePen(PS_SOLID, pw, col);
        HBRUSH br = CreateSolidBrush(col);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        if (which == 1) {
            /* seta circular: arco de 50° a 360° e ponta apontando para cima */
            int rad = S(6);
            POINT pts[40];
            int n = 0;
            for (int d = 50; d <= 360 && n < 40; d += 10) {
                double a = d * 3.14159265358979 / 180.0;
                pts[n].x = cx + (int)lround(rad * cos(a));
                pts[n].y = cy - (int)lround(rad * sin(a));
                n++;
            }
            Polyline(dc, pts, n);
            int a = S(3);
            POINT tri[3] = { { cx + rad - a, cy + 1 }, { cx + rad + a + 1, cy + 1 }, { cx + rad, cy - a } };
            Polygon(dc, tri, 3);
        } else {
            /* besouro: corpo, cabeça e três pernas de cada lado */
            Ellipse(dc, cx - S(3), cy - S(2), cx + S(3) + 1, cy + S(6));
            Ellipse(dc, cx - S(2), cy - S(6), cx + S(2) + 1, cy - S(1));
            for (int k = 0; k < 3; k++) {
                int yy = cy + k * S(2) - S(1);
                MoveToEx(dc, cx - S(3), yy, NULL); LineTo(dc, cx - S(6), yy - S(1) + k);
                MoveToEx(dc, cx + S(3), yy, NULL); LineTo(dc, cx + S(6), yy - S(1) + k);
            }
        }
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
        DeleteObject(br);
    }
    if (busy) reset_dc(dc);

    if (which == 2 && g_unseenErr > 0) { /* ponto vermelho: há erros que você ainda não viu */
        int bx = r->right - S(5), by = r->top + S(5), rr = S(3);
        HBRUSH b1 = CreateSolidBrush(C_BG), b2 = CreateSolidBrush(C_DANGER);
        HPEN np = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ op = SelectObject(dc, np), ob = SelectObject(dc, b1);
        Ellipse(dc, bx - rr - 1, by - rr - 1, bx + rr + 2, by + rr + 2);
        SelectObject(dc, b2);
        Ellipse(dc, bx - rr, by - rr, bx + rr + 1, by + rr + 1);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b1);
        DeleteObject(b2);
    }
}

/* dicas (tooltip) dos botões */
static void tips_update(void)
{
    if (!g_tip) return;
    RECT rc, rr, rd;
    GetClientRect(g_hwnd, &rc);
    header_buttons(rc.right, &rr, &rd);
    if (g_compact) { SetRectEmpty(&rr); SetRectEmpty(&rd); }
    TOOLINFOW ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.hwnd = g_hwnd;
    ti.uId = 1; ti.rect = rr;
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    ti.uId = 2; ti.rect = rd;
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

static void tips_create(void)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_hwnd, NULL, g_inst, NULL);
    if (!g_tip) return;
    static WCHAR t1[] = L"Atualizar limites agora (pergunta ao Claude Code)";
    static WCHAR t2[] = L"Debug: log, erros e mensagens internas";
    TOOLINFOW ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g_hwnd;
    ti.hinst = g_inst;
    ti.uId = 1; ti.lpszText = t1;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    ti.uId = 2; ti.lpszText = t2;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    tips_update();
}

static void paint(HDC dc, int w, int h)
{
    State s = snapshot();
    long long used = used_of(&s);
    double ratio = s.ctxWindow > 0 ? (double)used / (double)s.ctxWindow : 0.0;
    COLORREF col = status_color(ratio);
    WCHAR pct[32], a[32], b[32], c[32], line[160], model[128];

    fill_rect(dc, 0, 0, w, h, C_BG);
    SetBkMode(dc, TRANSPARENT);
    fmt_pct(pct, 32, ratio * 100.0);
    MultiByteToWideChar(CP_UTF8, 0, s.model, -1, model, 128);

    if (g_compact) {
        int cy = h / 2, r = S(4), cx = S(13);
        if (!(s.streaming && g_blink)) {
            HBRUSH br = CreateSolidBrush(col);
            HPEN pn = CreatePen(PS_SOLID, 1, col);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
            Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br); DeleteObject(pn);
        }
        WCHAR ct[96];
        compact_text(&s, ct, 96);
        draw_line(dc, g_fPill, C_FG, S(24), 0, w - S(4), h, ct, DT_VCENTER);
        return;
    }

    int L = S(12), R = w - S(12);

    /* cabeçalho */
    draw_line(dc, g_fTitle, C_MUTED, L, S(8), R, S(24), L"CONTEXTO", 0);
    if (s.streaming && !g_blink) {
        SIZE z;
        SelectObject(dc, g_fTitle);
        GetTextExtentPoint32W(dc, L"CONTEXTO", 8, &z);
        int cx = L + z.cx + S(8), cy = S(8) + z.cy / 2, r = S(3);
        HBRUSH br = CreateSolidBrush(col);
        HPEN pn = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(br); DeleteObject(pn);
    }
    RECT rbR, rbD;
    header_buttons(w, &rbR, &rbD);
    draw_line(dc, g_fSmall, C_MUTED, L + S(92), S(8), rbR.left - S(4), S(24), model, DT_RIGHT);
    draw_header_button(dc, &rbR, 1);
    draw_header_button(dc, &rbD, 2);

    /* porcentagem grande + usado/total */
    TEXTMETRICW tmBig, tmSmall;
    SIZE zp;
    SelectObject(dc, g_fBig);
    GetTextMetricsW(dc, &tmBig);
    GetTextExtentPoint32W(dc, pct, (int)wcslen(pct), &zp);
    SetTextColor(dc, col);
    int yBig = S(22);
    TextOutW(dc, L, yBig, pct, (int)wcslen(pct));
    SelectObject(dc, g_fSmall);
    GetTextMetricsW(dc, &tmSmall);
    fmt_int(a, 32, used);
    fmt_compact(b, 32, s.ctxWindow);
    _snwprintf(line, 159, L"%s / %s", a, b);
    line[159] = 0;
    draw_line(dc, g_fSmall, C_MUTED, L + zp.cx + S(8), yBig + tmBig.tmAscent - tmSmall.tmAscent, R,
              yBig + tmBig.tmHeight, line, 0);

    /* barra */
    int bt = S(62), bb = bt + S(6), bw = R - L, rad = S(6);
    fill_round(dc, L, bt, R, bb, rad, C_TRACK);
    if (used > 0) {
        int fw = (int)(bw * (ratio > 1.0 ? 1.0 : ratio));
        if (fw < bb - bt) fw = bb - bt;
        fill_round(dc, L, bt, L + fw, bb, rad, col);
    }
    int t1 = L + bw * g_warn / 100, t2 = L + bw * g_danger / 100;
    fill_rect(dc, t1, bt, t1 + S(1), bb, C_BG);
    fill_rect(dc, t2, bt, t2 + S(1), bb, C_BG);

    /* detalhamento */
    fmt_compact(a, 32, s.cur.input);
    fmt_compact(b, 32, s.cur.cacheRead + s.cur.cacheWrite);
    fmt_compact(c, 32, s.cur.output);
    _snwprintf(line, 159, L"Entrada %s  ·  Cache %s  ·  Saída %s", a, b, c);
    line[159] = 0;
    draw_line(dc, g_fSmall, C_FG, L, S(75), R, S(93), line, 0);

    int y = S(93);
    if (show_session_line(&s)) {
        fmt_compact(a, 32, s.sesIn);
        fmt_compact(b, 32, s.sesOut);
        _snwprintf(line, 159, L"%d %s  ·  %s in  ·  %s out", s.calls,
                   s.calls == 1 ? L"chamada" : L"chamadas", a, b);
        line[159] = 0;
        draw_line(dc, g_fSmall, C_MUTED, L, y, R, y + S(18), line, 0);
        y += S(18);
    }

    /* limites */
    long long now = now_epoch();
    Row rows[8];
    int n = build_rows(&s, rows, now);
    WCHAR ccs[160];
    int ccl;
    EnterCriticalSection(&g_cs);
    wcscpy(ccs, g_ccStatus);
    ccl = g_ccLevel;
    LeaveCriticalSection(&g_cs);
    y += S(6);
    fill_rect(dc, L, y, R, y + S(1), C_TRACK);
    y += S(8);
    if (!n) { /* ainda sem limites: mostra o que está acontecendo */
        const WCHAR *txt = wcsncmp(ccs, L"Limites Pro/Max: ", 17) ? ccs : ccs + 17;
        WCHAR full[200];
        _snwprintf(full, 199, L"Limites: %s", txt);
        full[199] = 0;
        draw_line(dc, g_fSmall, g_refreshing ? C_MUTED : ccl == LV_ERR ? C_DANGER : ccl == LV_WARN ? C_WARN : C_MUTED,
                  L, y, R, y + S(18), full, DT_VCENTER);
        return;
    }
    draw_line(dc, g_fTitle, C_MUTED, L, y, R, y + S(16), L"LIMITES", 0);
    if (g_refreshing) {
        draw_line(dc, g_fSmall, C_MUTED, L + S(70), y, R, y + S(16), L"consultando\u2026", DT_RIGHT);
    } else if (ccl == LV_ERR) {
        draw_line(dc, g_fSmall, C_DANGER, L + S(70), y, R, y + S(16), L"erro na \u00FAltima consulta \u00B7 ver debug", DT_RIGHT);
    } else if (s.subUpdated) {
        long long age = now - s.subUpdated;
        WCHAR ag[32];
        WCHAR plan[24] = L"";
        if (s.plan[0]) {
            MultiByteToWideChar(CP_UTF8, 0, s.plan, -1, plan, 16);
            plan[0] = towupper(plan[0]);
            wcscat(plan, L" \u00B7 ");
        }
        if (age < 60) _snwprintf(line, 159, L"%satualizado agora", plan);
        else { fmt_left(ag, 32, age); _snwprintf(line, 159, L"%satualizado h\u00E1 %s", plan, ag); }
        line[159] = 0;
        draw_line(dc, g_fSmall, C_MUTED, L + S(70), y, R, y + S(16), line, DT_RIGHT);
    }
    y += S(18);

    for (int i = 0; i < n; i++, y += S(20)) {
        Row *r = &rows[i];
        int yb = y + S(18);
        draw_line(dc, g_fSmall, C_FG, L, y, L + S(56), yb, r->label, DT_VCENTER);
        if (r->pct >= 0) {
            WCHAR pc[16];
            _snwprintf(pc, 15, L"%.0f%%", r->pct);
            pc[15] = 0;
            draw_line(dc, g_fSmall, r->col, L + S(54), y, L + S(92), yb, pc, DT_VCENTER | DT_RIGHT);
        } else {
            draw_line(dc, g_fSmall, C_MUTED, L + S(54), y, L + S(92), yb, L"\u2014", DT_VCENTER | DT_RIGHT);
        }
        int bl = L + S(100), br = L + S(146), bt2 = y + S(7), bb2 = bt2 + S(5);
        fill_round(dc, bl, bt2, br, bb2, S(5), C_TRACK);
        if (r->pct > 0) {
            int fw = (int)((br - bl) * (r->pct > 100 ? 1.0 : r->pct / 100.0));
            if (fw < bb2 - bt2) fw = bb2 - bt2;
            fill_round(dc, bl, bt2, bl + fw, bb2, S(5), r->col);
        }
        int right = R;
        if (r->clock[0]) {
            SIZE z;
            SelectObject(dc, g_fSmall);
            GetTextExtentPoint32W(dc, r->clock, (int)wcslen(r->clock), &z);
            draw_line(dc, g_fSmall, C_MUTED, R - z.cx, y, R, yb, r->clock, DT_VCENTER);
            right = R - z.cx - S(8);
        }
        draw_line(dc, g_fSmall, C_FG, L + S(152), y, right, yb, r->left, DT_VCENTER | DT_RIGHT);
    }
}

/* ícone da bandeja: pizza que enche conforme o uso */
static HICON make_gauge_icon(double ratio, COLORREF col)
{
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;

    DWORD *px = (DWORD *)bits;
    double cx = (sz - 1) / 2.0, R = sz / 2.0 - 0.5;
    const double TAU = 6.283185307179586;
    if (ratio > 1.0) ratio = 1.0;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            double dx = x - cx, dy = y - cx, d = sqrt(dx * dx + dy * dy);
            double cov = R + 0.5 - d;
            if (cov <= 0) { px[y * sz + x] = 0; continue; }
            if (cov > 1) cov = 1;
            double ang = atan2(dx, -dy);
            if (ang < 0) ang += TAU;
            int in = ratio > 0 && ang / TAU <= ratio;
            COLORREF cc = in ? col : RGB(0x9A, 0x9A, 0xA2);
            double al = (in ? 255.0 : 120.0) * cov;
            BYTE A = (BYTE)al;
            BYTE r = (BYTE)(GetRValue(cc) * al / 255.0);
            BYTE gg = (BYTE)(GetGValue(cc) * al / 255.0);
            BYTE bb = (BYTE)(GetBValue(cc) * al / 255.0);
            px[y * sz + x] = ((DWORD)A << 24) | ((DWORD)r << 16) | ((DWORD)gg << 8) | bb;
        }
    }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

static void update_tray(int force)
{
    State s = snapshot();
    long long used = used_of(&s);
    double ratio = s.ctxWindow > 0 ? (double)used / (double)s.ctxWindow : 0.0;
    int bucket = (int)(ratio * 200.0); /* redesenha a cada 0,5% */
    WCHAR pct[32], a[32], b[32];
    fmt_pct(pct, 32, ratio * 100.0);
    fmt_int(a, 32, used);
    fmt_int(b, 32, s.ctxWindow);
    _snwprintf(g_nid.szTip, 127, L"Contexto %s (%s / %s)", pct, a, b);
    g_nid.szTip[127] = 0;
    {
        long long now = now_epoch();
        Row rows[8];
        int n = build_rows(&s, rows, now);
        for (int i = 0; i < n; i++) {
            WCHAR ln[64];
            if (rows[i].pct >= 0)
                _snwprintf(ln, 63, L"\n%s %.0f%% · %s", rows[i].label, rows[i].pct, rows[i].clock[0] ? rows[i].clock : rows[i].left);
            else
                _snwprintf(ln, 63, L"\n%s · %s", rows[i].label, rows[i].clock[0] ? rows[i].clock : rows[i].left);
            ln[63] = 0;
            if (wcslen(g_nid.szTip) + wcslen(ln) < 127) wcscat(g_nid.szTip, ln);
        }
    }
    (void)force; (void)bucket;   /* o desenho do ícone fica com a animação (icon_frame) */
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = make_gauge_icon(0, C_OK);
    g_nid.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayBucket = -1;
    update_tray(1);
}

static void set_visible(int v)
{
    g_visible = v;
    ShowWindow(g_hwnd, v ? SW_SHOWNOACTIVATE : SW_HIDE);
}

/* ------------------------------------------------------------------ */
/* ícone animado na barra de tarefas                                    */
/*                                                                     */
/* Anel que enche conforme o percentual, com o número no meio. Quando   */
/* o valor muda, o anel desliza até o novo valor; enquanto consulta ou  */
/* recebe streaming, um brilho gira em volta; acima do limite vermelho, */
/* o anel pulsa. O botão também ganha a barra de progresso nativa do    */
/* Windows (verde / amarelo / vermelho).                                */
/* ------------------------------------------------------------------ */
enum { SRC_MAX = 0, SRC_CONTEXT, SRC_5H, SRC_WEEK, SRC_OFF };
static const WCHAR *SRC_KEYS[] = { L"max", L"context", L"5h", L"week", L"off" };
static int g_iconSrc = SRC_MAX;
static double g_icoShown = -1, g_icoTarget = 0;
static unsigned g_icoPhase = 0;
static UINT g_msgTaskbarBtn;
static ITaskbarList3 *g_tbl;
static HICON g_icoBig, g_icoSmall;
static WCHAR g_title[160];
static int g_tbState = -1, g_tbValue = -1;
#define TIMER_ICON 5

/* percentual que o ícone mostra, conforme a escolha no menu */
static double icon_ratio(const State *s, const WCHAR **label)
{
    long long now = now_epoch();
    double ctx = s->ctxWindow > 0 ? (double)used_of(s) / (double)s->ctxWindow : 0;
    double f5 = !s->fiveH.has ? -1 : s->fiveH.resets > now ? s->fiveH.pct / 100.0 : 0;
    double wk = !s->sevenD.has ? -1 : s->sevenD.resets > now ? s->sevenD.pct / 100.0 : 0;
    double r = ctx;
    const WCHAR *l = L"contexto";
    switch (g_iconSrc) {
    case SRC_5H: if (f5 >= 0) { r = f5; l = L"5 horas"; } break;
    case SRC_WEEK: if (wk >= 0) { r = wk; l = L"semana"; } break;
    case SRC_CONTEXT: break;
    default:
        if (f5 > r) { r = f5; l = L"5 horas"; }
        if (wk > r) { r = wk; l = L"semana"; }
    }
    if (label) *label = l;
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    return r;
}

static COLORREF lighten(COLORREF c, double t)
{
    if (t <= 0) return c;
    if (t > 1) t = 1;
    return RGB(GetRValue(c) + (int)((255 - GetRValue(c)) * t),
               GetGValue(c) + (int)((255 - GetGValue(c)) * t),
               GetBValue(c) + (int)((255 - GetBValue(c)) * t));
}

/* pinta uma camada (cor, alfa 0..1) por cima do pixel (BGRA pré-multiplicado) */
static void px_over(DWORD *p, COLORREF c, double a)
{
    if (a <= 0) return;
    if (a > 1) a = 1;
    double ia = 1 - a;
    DWORD v = *p;
    double A = ((v >> 24) & 255) * ia + 255 * a;
    double R = ((v >> 16) & 255) * ia + GetRValue(c) * a;
    double G = ((v >> 8) & 255) * ia + GetGValue(c) * a;
    double B = (v & 255) * ia + GetBValue(c) * a;
    *p = ((DWORD)(A + .5) << 24) | ((DWORD)(R + .5) << 16) | ((DWORD)(G + .5) << 8) | (DWORD)(B + .5);
}

static HFONT ring_font(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

/* desenha o anel em px (sz x sz). glow: 0..1 clareia o anel; comet: ângulo (0..1) do brilho que gira, < 0 sem brilho */
static void render_ring(DWORD *px, int sz, double ratio, COLORREF col, double glow, double comet, int number)
{
    const double TAU = 6.283185307179586;
    double c = (sz - 1) / 2.0, R = sz / 2.0 - 0.25;
    double T = sz * (number ? 0.17 : 0.24);
    if (T < 2.2) T = 2.2;
    double Ri = R - T;
    COLORREF arc = lighten(col, glow);
    memset(px, 0, (size_t)sz * sz * sizeof(DWORD));

    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            double dx = x - c, dy = y - c, d = sqrt(dx * dx + dy * dy);
            DWORD *p = &px[y * sz + x];
            if (number) { /* miolo escuro para o número aparecer em qualquer fundo */
                double cd = Ri + 0.4 - d;
                if (cd > 0) px_over(p, RGB(0x1F, 0x1F, 0x22), (cd > 1 ? 1 : cd) * 0.92);
            }
            double co = R + 0.5 - d, ci = d - (Ri - 0.5);
            if (co <= 0 || ci <= 0) continue;
            double cov = (co > 1 ? 1 : co) * (ci > 1 ? 1 : ci);
            double ang = atan2(dx, -dy);
            if (ang < 0) ang += TAU;
            double f = ang / TAU;
            /* borda suave no fim do arco */
            double edge = (ratio - f) * TAU * ((R + Ri) / 2) + 0.5;
            if (edge > 1) edge = 1;
            if (ratio <= 0) edge = 0;
            px_over(p, RGB(0x80, 0x80, 0x88), cov * 0.45);
            if (edge > 0) px_over(p, arc, cov * edge);
            if (comet >= 0) {
                double dd = comet - f;
                dd -= floor(dd);           /* distância angular atrás da cabeça do brilho */
                double tail = 1 - dd / 0.22;
                if (tail > 0) px_over(p, RGB(255, 255, 255), cov * tail * tail * 0.85);
            }
        }
    }

    if (number) { /* número: desenhado em tons de cinza e usado como máscara */
        int pct = (int)floor(ratio * 100 + 0.5);
        WCHAR t[8];
        _snwprintf(t, 7, L"%d", pct);
        t[7] = 0;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = sz;
        bi.bmiHeader.biHeight = -sz;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = NULL;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bm && bits) {
            HGDIOBJ ob = SelectObject(dc, bm);
            memset(bits, 0, (size_t)sz * sz * 4);
            HFONT f = ring_font((int)(sz * (pct >= 100 ? 0.34 : 0.44)));
            HGDIOBJ of = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT rc = { 0, 0, sz, sz };
            DrawTextW(dc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GdiFlush();
            DWORD *m = (DWORD *)bits;
            for (int i = 0; i < sz * sz; i++) {
                double a = ((m[i] >> 8) & 255) / 255.0;
                if (a > 0) px_over(&px[i], RGB(255, 255, 255), a);
            }
            SelectObject(dc, of);
            DeleteObject(f);
            SelectObject(dc, ob);
            DeleteObject(bm);
        }
        DeleteDC(dc);
    }
}

static HICON icon_from_px(const DWORD *px, int sz)
{
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    memcpy(bits, px, (size_t)sz * sz * 4);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

static HICON make_ring_icon(int sz, double ratio, COLORREF col, double glow, double comet, int number)
{
    DWORD *px = (DWORD *)malloc((size_t)sz * sz * 4);
    if (!px) return NULL;
    render_ring(px, sz, ratio, col, glow, comet, number);
    HICON ic = icon_from_px(px, sz);
    free(px);
    return ic;
}

/* barra de progresso nativa no botão da barra de tarefas */
static void taskbar_progress(double ratio, int busy)
{
    if (!g_tbl || g_iconSrc == SRC_OFF) return;
    int st = busy ? TBPF_INDETERMINATE
           : ratio * 100 >= g_danger ? TBPF_ERROR
           : ratio * 100 >= g_warn ? TBPF_PAUSED
           : ratio > 0.0005 ? TBPF_NORMAL : TBPF_NOPROGRESS;
    int val = (int)(ratio * 1000 + 0.5);
    if (st != g_tbState) {
        ITaskbarList3_SetProgressState(g_tbl, g_hwnd, (TBPFLAG)st);
        g_tbState = st;
        g_tbValue = -1;
    }
    if (st != TBPF_INDETERMINATE && st != TBPF_NOPROGRESS && val != g_tbValue) {
        ITaskbarList3_SetProgressValue(g_tbl, g_hwnd, (ULONGLONG)val, 1000);
        g_tbValue = val;
    }
}

/* título da janela = texto do botão e da miniatura na barra de tarefas */
static void update_title(const State *s)
{
    long long now = now_epoch();
    WCHAR t[160], part[48];
    double ctx = s->ctxWindow > 0 ? used_of(s) * 100.0 / (double)s->ctxWindow : 0;
    _snwprintf(t, 159, L"TokenMeter · contexto %.0f%%", ctx);
    t[159] = 0;
    if (s->fiveH.has) {
        _snwprintf(part, 47, L" · 5h %.0f%%", s->fiveH.resets > now ? s->fiveH.pct : 0.0);
        part[47] = 0;
        wcscat(t, part);
    }
    if (s->sevenD.has) {
        _snwprintf(part, 47, L" · semana %.0f%%", s->sevenD.resets > now ? s->sevenD.pct : 0.0);
        part[47] = 0;
        wcscat(t, part);
    }
    if (wcscmp(t, g_title)) {
        wcscpy(g_title, t);
        SetWindowTextW(g_hwnd, t);
    }
}

/* um quadro da animação: ícones grande/pequeno, bandeja e progresso */
static void icon_frame(int pulseOn, int busy)
{
    double r = g_icoShown < 0 ? 0 : g_icoShown;
    COLORREF col = status_color(r);
    double glow = 0, comet = -1;
    if (busy) comet = fmod(g_icoPhase * 0.06, 1.0);
    else if (pulseOn) glow = 0.45 * (0.5 - 0.5 * cos(g_icoPhase * 0.25));  /* "respira" */

    if (g_iconSrc != SRC_OFF) {
        int big = GetSystemMetrics(SM_CXICON), smallSz = GetSystemMetrics(SM_CXSMICON);
        if (big < 32) big = 32;
        if (smallSz < 16) smallSz = 16;
        HICON b = make_ring_icon(big, r, col, glow, comet, 1);
        HICON s = make_ring_icon(smallSz, r, col, glow, comet, 0);
        if (b) { SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)b); if (g_icoBig) DestroyIcon(g_icoBig); g_icoBig = b; }
        if (s) { SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)s); if (g_icoSmall) DestroyIcon(g_icoSmall); g_icoSmall = s; }
        taskbar_progress(r, busy);
    }
    /* bandeja: menos quadros (a área de notificação não gosta de ser redesenhada sem parar) */
    if (g_icoPhase % 3 == 0 || (!pulseOn && !busy)) {
        int tsz = GetSystemMetrics(SM_CXSMICON);
        if (tsz < 16) tsz = 16;
        HICON t = make_ring_icon(tsz, r, col, glow, comet, 0);
        if (t) {
            g_nid.uFlags = NIF_ICON;
            g_nid.hIcon = t;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            if (g_trayIcon) DestroyIcon(g_trayIcon);
            g_trayIcon = t;
        }
    }
}

/* recalcula o alvo e liga a animação, se precisar */
static void icon_update(int force)
{
    State s = snapshot();
    g_icoTarget = icon_ratio(&s, NULL);
    update_title(&s);
    int busy = g_refreshing || s.streaming;
    int pulse = g_icoTarget * 100 >= g_danger;
    if (g_icoShown < 0) { g_icoShown = g_icoTarget; force = 1; }
    if (fabs(g_icoTarget - g_icoShown) > 0.0005 || busy || pulse) {
        SetTimer(g_hwnd, TIMER_ICON, 50, NULL);
    } else if (force) {
        icon_frame(0, 0);
    }
}

static void icon_tick(void)
{
    State s = snapshot();
    int busy = g_refreshing || s.streaming;
    double d = g_icoTarget - g_icoShown;
    int moving = fabs(d) > 0.0005;
    if (moving) {
        double step = d * 0.18;  /* desacelera perto do alvo */
        if (fabs(step) < 0.002) step = d > 0 ? (d < 0.002 ? d : 0.002) : (d > -0.002 ? d : -0.002);
        g_icoShown += step;
    }
    int pulse = g_icoShown * 100 >= g_danger;
    g_icoPhase++;
    icon_frame(pulse, busy);
    if (!moving && !busy && !pulse) {
        KillTimer(g_hwnd, TIMER_ICON);
        g_icoPhase = 0;
        icon_frame(0, 0);   /* quadro final, sem brilho */
    }
}

/* liga/desliga o botão na barra de tarefas */
static void apply_taskbar_style(void)
{
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    LONG_PTR want = g_iconSrc == SRC_OFF ? ((ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW)
                                         : ((ex | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW);
    if (want == ex) return;
    int vis = IsWindowVisible(g_hwnd);
    if (vis) ShowWindow(g_hwnd, SW_HIDE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, want);
    g_tbState = g_tbValue = -1;
    if (vis) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
}

static void set_icon_source(int src)
{
    g_iconSrc = src;
    WritePrivateProfileStringW(APP_NAME, L"taskbar_icon", SRC_KEYS[src], g_ini);
    apply_taskbar_style();
    logmsg(LV_INFO, L"Ícone da barra de tarefas: %s", SRC_KEYS[src]);
    icon_update(1);
}

/* ------------------------------------------------------------------ */
/* integração com o Claude Code (status line)                          */
/* ------------------------------------------------------------------ */
/* Comando para o settings.json. Barras normais (o Git Bash come as invertidas)
   e caminho curto 8.3 quando há espaços, para funcionar no Git Bash e no PowerShell. */
static void cc_command(WCHAR *out, size_t cap, int jsonEscape)
{
    WCHAR path[MAX_PATH], shortp[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (wcschr(path, L' ')) {
        DWORD n = GetShortPathNameW(path, shortp, MAX_PATH);
        if (n && n < MAX_PATH && !wcschr(shortp, L' ')) wcscpy(path, shortp);
    }
    for (WCHAR *p = path; *p; p++) if (*p == L'\\') *p = L'/';
    if (wcschr(path, L' '))
        _snwprintf(out, cap - 1, jsonEscape ? L"\\\"%s\\\" --statusline" : L"\"%s\" --statusline", path);
    else
        _snwprintf(out, cap - 1, L"%s --statusline", path);
    out[cap - 1] = 0;
}

static char *read_file_utf8(const WCHAR *path, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(f, NULL), got = 0;
    char *b = (sz != INVALID_FILE_SIZE && sz < 16 * 1024 * 1024) ? (char *)malloc(sz + 1) : NULL;
    if (b && (!ReadFile(f, b, sz, &got, NULL) || got != sz)) { free(b); b = NULL; }
    CloseHandle(f);
    if (b) { b[sz] = 0; *len = sz; }
    return b;
}

static int write_file_atomic(const WCHAR *path, const char *data, size_t n)
{
    WCHAR tmp[MAX_PATH + 8];
    _snwprintf(tmp, MAX_PATH + 7, L"%s.tmp", path);
    tmp[MAX_PATH + 7] = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    if (!ok) { DeleteFileW(tmp); return 0; }
    return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static void cc_setup(void)
{
    WCHAR home[MAX_PATH], dir[MAX_PATH], file[MAX_PATH], bak[MAX_PATH + 32], cmdW[MAX_PATH + 64], msg[1024];
    DWORD hn = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (!hn || hn >= MAX_PATH) { MessageBoxW(g_hwnd, L"Não achei a pasta do usuário.", APP_NAME, MB_SETFOREGROUND | MB_ICONERROR); return; }
    _snwprintf(dir, MAX_PATH - 1, L"%s\\.claude", home);
    dir[MAX_PATH - 1] = 0;
    _snwprintf(file, MAX_PATH - 1, L"%s\\settings.json", dir);
    file[MAX_PATH - 1] = 0;
    CreateDirectoryW(dir, NULL);

    cc_command(cmdW, MAX_PATH + 64, 1);
    char cmd[MAX_PATH * 3 + 64], obj[MAX_PATH * 3 + 160];
    WideCharToMultiByte(CP_UTF8, 0, cmdW, -1, cmd, sizeof cmd, NULL, NULL);
    snprintf(obj, sizeof obj, "{\n    \"type\": \"command\",\n    \"command\": \"%s\"\n  }", cmd);

    DWORD len = 0;
    char *old = read_file_utf8(file, &len);
    size_t cap = (old ? len : 0) + strlen(obj) + 256;
    char *out = (char *)malloc(cap);
    if (!out) { free(old); return; }

    const char *body = old ? old : "";
    int bom = old && len >= 3 && (unsigned char)old[0] == 0xEF && (unsigned char)old[1] == 0xBB && (unsigned char)old[2] == 0xBF;
    if (bom) body += 3;
    const char *first = skip_ws(body);

    if (!*first) {
        snprintf(out, cap, "{\n  \"statusLine\": %s\n}\n", obj);
    } else if (*first != '{') {
        MessageBoxW(g_hwnd, L"O settings.json do Claude Code não parece ser um JSON válido.\n"
                            L"Use \"Copiar comando da status line\" e configure à mão.", APP_NAME, MB_SETFOREGROUND | MB_ICONWARNING);
        free(old); free(out);
        return;
    } else {
        const char *sl = find_key(body, "statusLine");
        if (sl) {
            const char *end = value_end(sl);
            if (!end) {
                MessageBoxW(g_hwnd, L"Não entendi a statusLine atual. Configure à mão (menu: Copiar comando).", APP_NAME, MB_SETFOREGROUND | MB_ICONWARNING);
                free(old); free(out);
                return;
            }
            size_t span = (size_t)(end - sl);
            char *cur = (char *)malloc(span + 1);
            if (cur) { memcpy(cur, sl, span); cur[span] = 0; }
            int ours = cur && strstr(cur, "--statusline") && (strstr(cur, "TokenMeter") || strstr(cur, "TOKENM"));
            free(cur);
            if (ours) {
                logmsg(LV_INFO, L"Conectar ao Claude Code: j\u00E1 estava conectado (%s)", file);
                MessageBoxW(g_hwnd, L"O Claude Code já está conectado ao TokenMeter.", APP_NAME, MB_SETFOREGROUND | MB_ICONINFORMATION);
                free(old); free(out);
                return;
            }
            if (MessageBoxW(g_hwnd, L"Você já tem uma status line configurada no Claude Code.\n\n"
                                    L"Substituir pela do TokenMeter? (uma cópia de segurança do arquivo será salva)",
                            APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                free(old); free(out);
                return;
            }
            size_t pre = (size_t)(sl - body);
            snprintf(out, cap, "%.*s%s%s", (int)pre, body, obj, end);
        } else {
            const char *after = skip_ws(first + 1);
            size_t pre = (size_t)(first + 1 - body);
            snprintf(out, cap, "%.*s\n  \"statusLine\": %s%s\n  %s", (int)pre, body, obj,
                     *after == '}' ? "" : ",", after);
        }
    }

    if (old) {
        _snwprintf(bak, MAX_PATH + 31, L"%s.antes-do-tokenmeter", file);
        bak[MAX_PATH + 31] = 0;
        CopyFileW(file, bak, FALSE);
    }
    int ok = write_file_atomic(file, out, strlen(out));
    if (ok) logmsg(LV_INFO, L"Status line do Claude Code configurada em %s (comando: %s)", file, cmdW);
    else { WCHAR e[256]; last_error_text(e, 256, GetLastError()); logmsg(LV_ERR, L"N\u00E3o consegui gravar %s: %s", file, e); }
    free(old);
    free(out);
    if (!ok) {
        _snwprintf(msg, 1023, L"Não consegui gravar:\n%s", file);
        msg[1023] = 0;
        MessageBoxW(g_hwnd, msg, APP_NAME, MB_SETFOREGROUND | MB_ICONERROR);
        return;
    }
    _snwprintf(msg, 1023,
               L"Pronto! O Claude Code vai mandar os limites para cá.\n\n"
               L"Abra uma sessão nova do Claude Code (logado com a conta Pro/Max). "
               L"Os limites de 5 horas e da semana aparecem depois da primeira resposta.\n\n"
               L"Arquivo alterado:\n%s", file);
    msg[1023] = 0;
    MessageBoxW(g_hwnd, msg, APP_NAME, MB_SETFOREGROUND | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ */
/* consulta automática dos limites Pro/Max ao Claude Code              */
/*                                                                     */
/* Abre o Claude Code escondido, no modo que o Agent SDK usa            */
/* (-p --input-format stream-json), e faz o pedido "get_usage": os      */
/* dados do comando /usage. Não envia mensagem ao modelo nem gasta uso. */
/* ------------------------------------------------------------------ */
static int g_refreshMin = 5;
static volatile LONG g_refreshing = 0;
static WCHAR g_ccStatus[160] = L"Limites Pro/Max: aguardando consulta";

static int g_ccLevel = LV_INFO;     /* gravidade do último status (para a linha na janela) */
static long long g_ccWhen = 0;

static void set_cc_status(int level, const WCHAR *fmt, const WCHAR *arg)
{
    WCHAR t[160];
    _snwprintf(t, 159, fmt, arg ? arg : L"");
    t[159] = 0;
    EnterCriticalSection(&g_cs);
    wcscpy(g_ccStatus, t);
    g_ccLevel = level;
    g_ccWhen = now_epoch();
    LeaveCriticalSection(&g_cs);
    /* o detalhe já foi para o log antes de cada chamada; aqui só muda o status */
}

/* Procura o Claude Code: claude_path no .ini, PATH, instalador nativo, npm. */
static int find_claude(WCHAR *out, int *isCmd)
{
    WCHAR p[MAX_PATH], home[MAX_PATH];
    *isCmd = 0;
    GetPrivateProfileStringW(APP_NAME, L"claude_path", L"", p, MAX_PATH, g_ini);
    if (p[0] && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
        size_t n = wcslen(p);
        *isCmd = n > 4 && (!_wcsicmp(p + n - 4, L".cmd") || !_wcsicmp(p + n - 4, L".bat"));
        wcscpy(out, p);
        return 1;
    }
    if (SearchPathW(NULL, L"claude.exe", NULL, MAX_PATH, out, NULL)) return 1;
    DWORD hn = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (hn && hn < MAX_PATH) {
        _snwprintf(p, MAX_PATH - 1, L"%s\\.local\\bin\\claude.exe", home);
        p[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { wcscpy(out, p); return 1; }
    }
    if (SearchPathW(NULL, L"claude.cmd", NULL, MAX_PATH, out, NULL)) { *isCmd = 1; return 1; }
    return 0;
}

static void apply_usage_response(const char *line, DWORD ms)
{
    long long now = now_epoch();
    const char *av = find_key(line, "rate_limits_available");
    if (strstr(line, "\"subtype\":\"error\"") && !av) {
        log_utf8(LV_ERR, L"Claude Code recusou o pedido get_usage: ", line, 400);
        set_cc_status(LV_ERR, L"Limites Pro/Max: atualize o Claude Code (claude update)", NULL);
        return;
    }
    if (!av || strncmp(av, "true", 4) != 0) {
        const char *st = find_key(line, "subscription_type");
        char plan[32] = "nenhum";
        if (st) read_str(st, plan, sizeof plan);
        WCHAR wp[32];
        MultiByteToWideChar(CP_UTF8, 0, plan, -1, wp, 32);
        logmsg(LV_WARN, L"Claude Code respondeu em %lu ms, mas sem limites de assinatura (rate_limits_available=false, plano: %s). "
                        L"Ele est\u00E1 logado com API key ou sem login.", ms, wp);
        set_cc_status(LV_ERR, L"Limites Pro/Max: o Claude Code n\u00E3o est\u00E1 logado com Pro/Max", NULL);
        return;
    }
    char big[8192], plan[16] = "";
    const char *st = find_key(line, "subscription_type");
    if (st) read_str(st, plan, sizeof plan);
    const char *rl = find_key(line, "rate_limits");
    int got = 0;
    Window f = {0}, w = {0};
    if (rl && *rl == '{' && copy_obj(rl, big, sizeof big)) {
        log_utf8(LV_INFO, L"rate_limits recebidos: ", big, 500);
        read_window(big, "five_hour", &f);
        read_window(big, "seven_day", &w);
    }
    EnterCriticalSection(&g_cs);
    if (f.has) { g.fiveH = f; got = 1; }
    if (w.has) { g.sevenD = w; got = 1; }
    if (rl) { g.subUpdated = now; strcpy(g.plan, plan); }
    LeaveCriticalSection(&g_cs);

    WCHAR c5[32] = L"\u2014", c7[32] = L"\u2014", wp[16];
    if (f.has && f.resets > now) fmt_clock(c5, 32, f.resets, now);
    if (w.has && w.resets > now) fmt_clock(c7, 32, w.resets, now);
    MultiByteToWideChar(CP_UTF8, 0, plan, -1, wp, 16);
    if (got) {
        logmsg(LV_INFO, L"Consulta ok em %lu ms \u00B7 plano %s \u00B7 5h: %.0f%% (reseta %s) \u00B7 semana: %.0f%% (reseta %s)",
               ms, wp, f.pct, c5, w.pct, c7);
        WCHAR hm[16];
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        _snwprintf(hm, 15, L"%02d:%02d", lt.wHour, lt.wMinute);
        hm[15] = 0;
        EnterCriticalSection(&g_cs);
        _snwprintf(g_ccStatus, 159, L"Limites Pro/Max: atualizados \u00E0s %s", hm);
        g_ccStatus[159] = 0;
        g_ccLevel = LV_INFO;
        g_ccWhen = now;
        LeaveCriticalSection(&g_cs);
    } else {
        log_utf8(LV_WARN, L"Resposta sem five_hour/seven_day: ", line, 500);
        set_cc_status(LV_WARN, L"Limites Pro/Max: sem dados nesta consulta", NULL);
    }
}

static void do_refresh(void)
{
    WCHAR exe[MAX_PATH], app[MAX_PATH], cmd[MAX_PATH * 2 + 256], cwd[MAX_PATH], err[256];
    int isCmd = 0;
    if (!find_claude(exe, &isCmd)) {
        logmsg(LV_ERR, L"Claude Code n\u00E3o encontrado: procurei claude.exe no PATH, em %%USERPROFILE%%\\.local\\bin "
                       L"e claude.cmd (npm). Se estiver em outro lugar, informe claude_path no .ini.");
        set_cc_status(LV_ERR, L"Limites Pro/Max: Claude Code n\u00E3o encontrado", NULL);
        return;
    }
    const WCHAR *args = L"-p --output-format stream-json --verbose --input-format stream-json --no-session-persistence";
    if (isCmd) {
        if (!GetEnvironmentVariableW(L"ComSpec", app, MAX_PATH)) wcscpy(app, L"C:\\Windows\\System32\\cmd.exe");
        _snwprintf(cmd, MAX_PATH * 2 + 255, L"cmd.exe /d /s /c \"\"%s\" %s\"", exe, args);
    } else {
        wcscpy(app, exe);
        _snwprintf(cmd, MAX_PATH * 2 + 255, L"\"%s\" %s", exe, args);
    }
    cmd[MAX_PATH * 2 + 255] = 0;
    /* pasta vazia: nada de hooks ou servidores MCP de algum projeto */
    DWORD tn = GetTempPathW(MAX_PATH - 16, cwd);
    if (!tn || tn >= MAX_PATH - 16) wcscpy(cwd, L"C:\\");
    else { wcscat(cwd, L"TokenMeter"); CreateDirectoryW(cwd, NULL); }
    logmsg(LV_INFO, L"Consultando o Claude Code: %s", cmd);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return;
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = outW;   /* mensagens de erro do Claude Code vão para o log */
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = CreateProcessW(app, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    DWORD cpErr = ok ? 0 : GetLastError();
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW); CloseHandle(outR);
        last_error_text(err, 256, cpErr);
        logmsg(LV_ERR, L"Falha ao iniciar o Claude Code: %s", err);
        set_cc_status(LV_ERR, L"Limites Pro/Max: n\u00E3o consegui abrir o Claude Code", NULL);
        return;
    }
    logmsg(LV_INFO, L"Processo iniciado (pid %lu); enviando initialize + get_usage", pi.dwProcessId);

    static const char req[] =
        "{\"request_id\":\"tm-init\",\"type\":\"control_request\",\"request\":{\"subtype\":\"initialize\"}}\n"
        "{\"request_id\":\"tm-usage\",\"type\":\"control_request\",\"request\":{\"subtype\":\"get_usage\",\"skip_behaviors\":true}}\n";
    DWORD wr = 0;
    WriteFile(inW, req, sizeof req - 1, &wr, NULL);

    size_t cap = 256 * 1024, len = 0;
    char *buf = (char *)malloc(cap + 1);
    char *line = NULL;
    int timedOut = 1;
    ULONGLONG deadline = t0 + 60000;
    if (buf) buf[0] = 0;
    while (buf && GetTickCount64() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(outR, NULL, 0, NULL, &avail, NULL)) { timedOut = 0; break; }   /* processo fechou */
        if (!avail) { Sleep(50); continue; }
        if (len + avail > cap) {
            if (cap >= 16 * 1024 * 1024) { timedOut = 0; break; }
            size_t nc = cap * 2;
            while (nc < len + avail) nc *= 2;
            char *nb = (char *)realloc(buf, nc + 1);
            if (!nb) break;
            buf = nb;
            cap = nc;
        }
        DWORD got = 0;
        if (!ReadFile(outR, buf + len, avail, &got, NULL) || !got) { timedOut = 0; break; }
        len += got;
        buf[len] = 0;
        char *m = strstr(buf, "\"request_id\":\"tm-usage\"");
        if (m && strchr(m, '\n')) {
            char *st = m;
            while (st > buf && st[-1] != '\n') st--;
            *strchr(m, '\n') = 0;
            line = st;
            timedOut = 0;
            break;
        }
    }
    DWORD ms = (DWORD)(GetTickCount64() - t0);
    CloseHandle(inW);  /* fim da entrada: o Claude Code encerra sozinho */
    DWORD exitCode = STILL_ACTIVE;
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
        logmsg(LV_WARN, L"O Claude Code n\u00E3o encerrou sozinho; finalizando o processo");
        TerminateProcess(pi.hProcess, 0);
    } else {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    CloseHandle(outR);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* linhas que não são JSON = mensagens do Claude Code (erros, avisos) */
    if (buf) {
        int shown = 0;
        char *end = line ? line : buf + len;
        for (char *p = buf; p < end && shown < 8;) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (n && *p != '{') {
                char tmp[400];
                size_t k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
                memcpy(tmp, p, k);
                tmp[k] = 0;
                log_utf8(LV_WARN, L"Sa\u00EDda do Claude Code: ", tmp, 380);
                shown++;
            } else if (n && (memmem_simple(p, n, "\"is_error\":true") || memmem_simple(p, n, "\"subtype\":\"error\""))) {
                char tmp[600];
                size_t k = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
                memcpy(tmp, p, k);
                tmp[k] = 0;
                log_utf8(LV_WARN, L"Erro informado pelo Claude Code: ", tmp, 500);
                shown++;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }

    if (line) {
        apply_usage_response(line, ms);
    } else if (timedOut) {
        logmsg(LV_ERR, L"O Claude Code n\u00E3o respondeu em 60 s (recebi %llu bytes).", (unsigned long long)len);
        set_cc_status(LV_ERR, L"Limites Pro/Max: o Claude Code n\u00E3o respondeu", NULL);
    } else {
        logmsg(LV_ERR, L"O Claude Code fechou sem responder ao get_usage (c\u00F3digo de sa\u00EDda %lu, %llu bytes recebidos). "
                       L"Rode \"claude\" num terminal para ver se pede login ou atualiza\u00E7\u00E3o.",
               exitCode, (unsigned long long)len);
        set_cc_status(LV_ERR, L"Limites Pro/Max: o Claude Code fechou sem responder", NULL);
    }
    free(buf);
}

static DWORD WINAPI refresh_thread(LPVOID unused)
{
    (void)unused;
    do_refresh();
    InterlockedExchange(&g_refreshing, 0);
    PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);
    return 0;
}

#define TIMER_ANIM 4
static int g_spin = 0;   /* ângulo do ícone ↻ durante a consulta */

static void start_refresh(const WCHAR *why)
{
    if (InterlockedCompareExchange(&g_refreshing, 1, 0) != 0) { /* já está consultando */
        logmsg(LV_INFO, L"Atualiza\u00E7\u00E3o %s ignorada: j\u00E1 h\u00E1 uma consulta em andamento", why);
        return;
    }
    logmsg(LV_INFO, L"Atualiza\u00E7\u00E3o dos limites %s", why);
    EnterCriticalSection(&g_cs);
    wcscpy(g_ccStatus, L"Limites Pro/Max: consultando o Claude Code\u2026");
    LeaveCriticalSection(&g_cs);
    HANDLE t = CreateThread(NULL, 0, refresh_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_refreshing, 0);
    if (g_hwnd) {
        SetTimer(g_hwnd, TIMER_ANIM, 50, NULL);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* janela de debug                                                     */
/* ------------------------------------------------------------------ */
enum { IDB_REFRESH = 200, IDB_COPY, IDB_CLEAR, IDB_OPENLOG, IDB_OPENINI };
static HWND g_dbgEdit, g_dbgBtn[5];
static HFONT g_dbgMono, g_dbgUi;

/* resumo do estado + log inteiro, como texto (CRLF para o controle EDIT) */
static WCHAR *dbg_build_text(void)
{
    size_t cap = 64 * 1024, n = 0;
    WCHAR *t = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!t) return NULL;
#define ADD(...) do { \
        WCHAR _l[1400]; _snwprintf(_l, 1399, __VA_ARGS__); _l[1399] = 0; \
        size_t _k = wcslen(_l); \
        if (n + _k + 3 > cap) { cap = (n + _k + 3) * 2; WCHAR *_nt = (WCHAR *)realloc(t, cap * sizeof(WCHAR)); if (!_nt) { t[n] = 0; return t; } t = _nt; } \
        memcpy(t + n, _l, _k * sizeof(WCHAR)); n += _k; t[n++] = L'\r'; t[n++] = L'\n'; t[n] = 0; \
    } while (0)

    State s = snapshot();
    long long now = now_epoch();
    WCHAR claude[MAX_PATH] = L"", st[160], a[32], b[32];
    int isCmd = 0, found = find_claude(claude, &isCmd);
    EnterCriticalSection(&g_cs);
    wcscpy(st, g_ccStatus);
    LeaveCriticalSection(&g_cs);

    ADD(L"=== TokenMeter " TM_VERSION_W L" \u2014 resumo ===");
    if (g_listen != INVALID_SOCKET) ADD(L"Servidor local:      http://127.0.0.1:%d (ok)", g_port);
    else ADD(L"Servidor local:      N\u00C3O EST\u00C1 RODANDO (porta %d ocupada?)", g_port);
    ADD(L"Configurações:       %s", g_ini);
    ADD(L"Arquivo de log:      %s", g_logPath);
    ADD(L"Claude Code:         %s", found ? claude : L"não encontrado (informe claude_path no .ini)");
    if (g_refreshMin > 0) ADD(L"Consulta automática: a cada %d min", g_refreshMin);
    else ADD(L"Consulta automática: desligada (refresh_minutes=0)");
    ADD(L"Status dos limites:  %s%s", st, g_refreshing ? L" (consultando agora)" : L"");
    if (s.subUpdated) { fmt_left(a, 32, now - s.subUpdated); ADD(L"Últimos limites:     há %s%s%S", a, s.plan[0] ? L" · plano " : L"", s.plan); }
    const Window *ws[] = { &s.fiveH, &s.sevenD, &s.spendGw };
    const WCHAR *nm[] = { L"5 horas", L"Semana ", L"Gasto  " };
    for (int i = 0; i < 3; i++) {
        if (!ws[i]->has) continue;
        if (ws[i]->resets > now) {
            fmt_left(a, 32, ws[i]->resets - now);
            fmt_clock(b, 32, ws[i]->resets, now);
            ADD(L"  %s:  %5.1f%%  reseta em %s (%s)", nm[i], ws[i]->pct, a, b);
        } else {
            ADD(L"  %s:  %5.1f%%  janela já virou (liberado)", nm[i], ws[i]->pct);
        }
    }
    if (s.apiHas) {
        fmt_int(a, 32, s.apiRemaining);
        fmt_int(b, 32, s.apiLimit);
        ADD(L"  API/min: %s de %s tokens disponíveis%s", a, b, s.spendCapHit ? L" · TETO MENSAL ATINGIDO" : L"");
    }
    fmt_int(a, 32, used_of(&s));
    fmt_int(b, 32, s.ctxWindow);
    WCHAR model[128] = L"\u2014";
    if (s.model[0]) MultiByteToWideChar(CP_UTF8, 0, s.model, -1, model, 128);
    ADD(L"Contexto:            %s / %s tokens \u00B7 modelo: %s \u00B7 %d chamadas", a, b, model, s.calls);
    ADD(L"");
    ADD(L"=== log (mais recente embaixo) ===");

    EnterCriticalSection(&g_logCs);
    for (int i = 0; i < g_logCount; i++) ADD(L"%s", g_log[(g_logHead + i) % LOG_MAX]);
    LeaveCriticalSection(&g_logCs);
    if (!g_logCount) ADD(L"(vazio)");
#undef ADD
    return t;
}

static void dbg_refresh_text(void)
{
    if (!g_dbgEdit) return;
    WCHAR *t = dbg_build_text();
    if (!t) return;
    SendMessageW(g_dbgEdit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_dbgEdit, t);
    int len = GetWindowTextLengthW(g_dbgEdit);
    SendMessageW(g_dbgEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_dbgEdit, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_dbgEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_dbgEdit, NULL, TRUE);
    free(t);
}

static void dbg_layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    UINT dpi = window_dpi(h);
    int pad = MulDiv(8, (int)dpi, 96), bh = MulDiv(28, (int)dpi, 96), x = pad;
    static const int widths[5] = { 170, 100, 90, 130, 150 };
    for (int i = 0; i < 5; i++) {
        int bw = MulDiv(widths[i], (int)dpi, 96);
        MoveWindow(g_dbgBtn[i], x, pad, bw, bh, TRUE);
        x += bw + pad / 2;
    }
    MoveWindow(g_dbgEdit, pad, pad * 2 + bh, rc.right - 2 * pad, rc.bottom - 3 * pad - bh, TRUE);
}

static LRESULT CALLBACK dbg_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        UINT dpi = window_dpi(h);
        g_dbgMono = CreateFontW(-MulDiv(12, (int)dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        g_dbgUi = CreateFontW(-MulDiv(12, (int)dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        static const WCHAR *labels[5] = { L"Atualizar limites agora", L"Copiar tudo", L"Limpar log",
                                          L"Abrir arquivo de log", L"Abrir configurações" };
        for (int i = 0; i < 5; i++) {
            g_dbgBtn[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          0, 0, 10, 10, h, (HMENU)(INT_PTR)(IDB_REFRESH + i), g_inst, NULL);
            SendMessageW(g_dbgBtn[i], WM_SETFONT, (WPARAM)g_dbgUi, TRUE);
        }
        g_dbgEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                    ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                    0, 0, 10, 10, h, NULL, g_inst, NULL);
        SendMessageW(g_dbgEdit, WM_SETFONT, (WPARAM)g_dbgMono, TRUE);
        SendMessageW(g_dbgEdit, EM_SETLIMITTEXT, 0, 0);
        dbg_layout(h);
        dbg_refresh_text();
        SetTimer(h, 1, 1000, NULL); /* resumo (contagens) a cada segundo, se nada mudar */
        return 0;
    }
    case WM_SIZE:
        dbg_layout(h);
        return 0;
    case WM_APP_LOG:
        SetTimer(h, 2, 150, NULL); /* junta várias linhas novas numa atualização só */
        return 0;
    case WM_TIMER:
        if (wp == 2) { KillTimer(h, 2); dbg_refresh_text(); }
        else if (wp == 1) {
            /* só redesenha sozinho se o usuário não estiver selecionando texto */
            DWORD a = 0, b = 0;
            SendMessageW(g_dbgEdit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if (a == b && GetFocus() != g_dbgEdit) dbg_refresh_text();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDB_REFRESH: start_refresh(L"pela janela de debug"); break;
        case IDB_COPY: {
            WCHAR *t = dbg_build_text();
            if (t) { copy_text(t); free(t); logmsg(LV_INFO, L"Texto do debug copiado para a área de transferência"); }
            break;
        }
        case IDB_CLEAR: log_clear(); dbg_refresh_text(); break;
        case IDB_OPENLOG: ShellExecuteW(h, L"open", L"notepad.exe", g_logPath, NULL, SW_SHOWNORMAL); break;
        case IDB_OPENINI: {
            HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL); /* cria se não existir */
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            ShellExecuteW(h, L"open", L"notepad.exe", g_ini, NULL, SW_SHOWNORMAL);
            break;
        }
        }
        return 0;
    case WM_CTLCOLORSTATIC: /* EDIT somente leitura: fundo branco, não cinza */
        if ((HWND)lp == g_dbgEdit) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_DESTROY:
        g_dbg = NULL;
        g_dbgEdit = NULL;
        if (g_dbgMono) { DeleteObject(g_dbgMono); g_dbgMono = NULL; }
        if (g_dbgUi) { DeleteObject(g_dbgUi); g_dbgUi = NULL; }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open_debug(void)
{
    InterlockedExchange(&g_unseenErr, 0);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    if (g_dbg) {
        ShowWindow(g_dbg, SW_RESTORE);
        SetForegroundWindow(g_dbg);
        return;
    }
    static int registered = 0;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = dbg_wndproc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        wc.lpszClassName = L"TokenMeterDebug";
        RegisterClassExW(&wc);
        registered = 1;
    }
    int w = MulDiv(860, (int)g_dpi, 96), h = MulDiv(520, (int)g_dpi, 96);
    g_dbg = CreateWindowExW(0, L"TokenMeterDebug", L"TokenMeter — debug", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, g_inst, NULL);
    if (g_dbg) {
        ShowWindow(g_dbg, SW_SHOWNORMAL);
        SetForegroundWindow(g_dbg);
    }
}

static void show_menu(void)
{
    POINT pt;
    GetCursorPos(&pt);
    State s = snapshot();
    WCHAR endpoint[96], winInfo[96], n[32];
    _snwprintf(endpoint, 95, L"Endereço: http://127.0.0.1:%d", g_port);
    endpoint[95] = 0;
    fmt_int(n, 32, s.ctxWindow);
    _snwprintf(winInfo, 95, L"Janela atual: %s tokens", n);
    winInfo[95] = 0;

    HMENU win = CreatePopupMenu();
    AppendMenuW(win, MF_STRING | MF_GRAYED, 0, winInfo);
    AppendMenuW(win, MF_SEPARATOR, 0, NULL);
    AppendMenuW(win, MF_STRING | (s.ctxWindow == 200000 ? MF_CHECKED : 0), IDM_WIN_200K, L"200.000 tokens");
    AppendMenuW(win, MF_STRING | (s.ctxWindow == 1000000 ? MF_CHECKED : 0), IDM_WIN_1M, L"1.000.000 tokens");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, endpoint);
    AppendMenuW(m, MF_STRING, IDM_COPY_URL, L"Copiar endereço");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RESET_CTX, L"Nova conversa (zerar contexto)");
    AppendMenuW(m, MF_STRING, IDM_RESET_ALL, L"Zerar tudo");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_compact ? MF_CHECKED : 0), IDM_COMPACT, L"Modo compacto");
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), IDM_TOPMOST, L"Sempre visível");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)win, L"Janela de contexto");
    {
        HMENU ti = CreatePopupMenu();
        static const WCHAR *names[5] = { L"O maior (contexto, 5 horas ou semana)", L"Contexto",
                                         L"Limite de 5 horas", L"Limite da semana", L"Sem bot\u00E3o na barra de tarefas" };
        for (int i = 0; i < 5; i++) {
            if (i == 4) AppendMenuW(ti, MF_SEPARATOR, 0, NULL);
            AppendMenuW(ti, MF_STRING | (g_iconSrc == i ? MF_CHECKED : 0), IDM_SRC_MAX + i, names[i]);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)ti, L"\u00CDcone da barra de tarefas");
    }
    AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Iniciar com o Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    {
        WCHAR st[160];
        EnterCriticalSection(&g_cs);
        wcscpy(st, g_ccStatus);
        LeaveCriticalSection(&g_cs);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, st);
    }
    AppendMenuW(m, MF_STRING | (g_refreshing ? MF_GRAYED : 0), IDM_REFRESH, L"Atualizar limites agora");
    AppendMenuW(m, MF_STRING, IDM_DEBUG, g_unseenErr ? L"Abrir debug (h\u00E1 erros novos)" : L"Abrir debug");
    AppendMenuW(m, MF_STRING, IDM_CC_SETUP, L"Conectar ao Claude Code (status line)");
    AppendMenuW(m, MF_STRING, IDM_CC_COPY, L"Copiar comando da status line");
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, g_visible ? L"Ocultar janela" : L"Mostrar janela");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Sair");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m); /* destrói o submenu junto */
}

static void set_window_size(long long n)
{
    EnterCriticalSection(&g_cs);
    g.ctxWindow = n;
    LeaveCriticalSection(&g_cs);
    ini_set(L"context_window", (int)n);
    InvalidateRect(g_hwnd, NULL, FALSE);
    update_tray(1);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) { tray_add(); icon_update(1); return 0; }
    if (msg == g_msgTaskbarBtn && g_msgTaskbarBtn) { /* o botão na barra de tarefas existe: já dá para pôr progresso */
        if (!g_tbl) {
            HRESULT hr = CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&g_tbl);
            if (SUCCEEDED(hr) && g_tbl && SUCCEEDED(ITaskbarList3_HrInit(g_tbl))) {
                logmsg(LV_INFO, L"Barra de tarefas pronta: progresso no bot\u00E3o ativado");
            } else {
                if (g_tbl) { ITaskbarList3_Release(g_tbl); g_tbl = NULL; }
                logmsg(LV_WARN, L"Barra de tarefas sem suporte a progresso (0x%08lX); s\u00F3 o \u00EDcone ser\u00E1 animado", (unsigned long)hr);
            }
        }
        g_tbState = g_tbValue = -1;
        icon_update(1);
        return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; /* não rouba o foco do seu programa */
    case WM_LBUTTONDOWN: {
        int btn = hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (btn) { /* clique num botão: não arrasta */
            g_press = btn;
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        POINT pt;
        RECT r;
        GetCursorPos(&pt);
        GetWindowRect(h, &r);
        g_dragOff.x = pt.x - r.left;
        g_dragOff.y = pt.y - r.top;
        g_dragging = 1;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            SetWindowPos(h, NULL, pt.x - g_dragOff.x, pt.y - g_dragOff.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            int hv = hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hv != g_hover) {
                g_hover = hv;
                InvalidateRect(h, NULL, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (g_hover) { g_hover = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        if (g_press) {
            int btn = hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), was = g_press;
            g_press = 0;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            if (btn == was) {
                if (btn == 1) start_refresh(L"pelo bot\u00E3o de atualizar");
                else open_debug();
            }
            return 0;
        }
        if (g_dragging) { g_dragging = 0; ReleaseCapture(); clamp_to_monitor(); save_pos(); }
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = 0;
        if (g_press) { g_press = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0; /* duplo clique num botão não muda o modo */
        g_compact = !g_compact;
        ini_set(L"compact", g_compact);
        resize_to_layout();
        save_pos();
        return 0;
    case WM_RBUTTONUP:
        show_menu();
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP) set_visible(!g_visible);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) show_menu();
        return 0;
    case WM_APP_UPDATE:
        fit_if_needed();
        InvalidateRect(h, NULL, FALSE);
        update_tray(0);
        icon_update(0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_FIRST) { KillTimer(h, TIMER_FIRST); start_refresh(L"ao abrir"); return 0; }
        if (wp == TIMER_REFRESH) { start_refresh(L"autom\u00E1tica"); return 0; }
        if (wp == TIMER_ICON) { icon_tick(); return 0; }
        if (wp == TIMER_ANIM) {
            if (!g_refreshing) { KillTimer(h, TIMER_ANIM); g_spin = 0; }
            else g_spin = (g_spin + 24) % 360;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        {
        int stale = 0, streaming;
        EnterCriticalSection(&g_cs);
        if (g.streaming && GetTickCount64() - g.lastEvent > STALE_MS) {
            g.streaming = 0; /* o programa parou de mandar eventos */
            commit_pending();
            stale = 1;
        }
        streaming = g.streaming;
        LeaveCriticalSection(&g_cs);
        g_blink = streaming ? !g_blink : 0;
        if (streaming || stale) InvalidateRect(h, NULL, FALSE);
        static unsigned ticks = 0;
        ticks++;
        if (ticks % 2 == 0) icon_update(0); /* começo/fim de consulta e streaming mudam a animação */
        if (ticks % 2 == 0) { /* 1 s: contagem regressiva dos limites */
            State st = snapshot();
            Row rows[8];
            if (build_rows(&st, rows, now_epoch()) || g_compact) {
                fit_if_needed();
                InvalidateRect(h, NULL, FALSE);
            }
        }
        if (ticks % 20 == 0) update_tray(0); /* 10 s: dica do ícone da bandeja */
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_COMPACT:
            g_compact = !g_compact;
            ini_set(L"compact", g_compact);
            resize_to_layout();
            save_pos();
            break;
        case IDM_TOPMOST:
            g_topmost = !g_topmost;
            ini_set(L"topmost", g_topmost);
            SetWindowPos(h, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case IDM_TOGGLE: set_visible(!g_visible); break;
        case IDM_RESET_CTX: reset_state(0); InvalidateRect(h, NULL, FALSE); update_tray(1); break;
        case IDM_RESET_ALL: reset_state(1); InvalidateRect(h, NULL, FALSE); update_tray(1); break;
        case IDM_WIN_200K: set_window_size(200000); break;
        case IDM_WIN_1M: set_window_size(1000000); break;
        case IDM_SRC_MAX: case IDM_SRC_CONTEXT: case IDM_SRC_5H: case IDM_SRC_WEEK: case IDM_SRC_OFF:
            set_icon_source(LOWORD(wp) - IDM_SRC_MAX);
            break;
        case IDM_AUTOSTART: autostart_set(!autostart_get()); break;
        case IDM_CC_SETUP: cc_setup(); break;
        case IDM_REFRESH: start_refresh(L"pelo menu"); break;
        case IDM_DEBUG: open_debug(); break;
        case IDM_CC_COPY: {
            WCHAR cmd[MAX_PATH + 64];
            cc_command(cmd, MAX_PATH + 64, 0);
            copy_text(cmd);
            break;
        }
        case IDM_COPY_URL: {
            WCHAR u[64];
            _snwprintf(u, 63, L"http://127.0.0.1:%d", g_port);
            u[63] = 0;
            copy_text(u);
            break;
        }
        case IDM_EXIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((LPCWSTR)lp, L"ImmersiveColorSet")) {
            read_theme();
            set_palette();
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        make_fonts();
        RECT *r = (RECT *)lp;
        int w, hh;
        layout_size(&w, &hh);
        SetWindowPos(h, NULL, r->left, r->top, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
        apply_corners();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        clamp_to_monitor();
        break;
    case WM_DESTROY:
        if (g_tbl) { ITaskbarList3_Release(g_tbl); g_tbl = NULL; }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void load_settings(void)
{
    WCHAR appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) GetTempPathW(MAX_PATH, appdata);
    _snwprintf(g_ini, MAX_PATH - 1, L"%s\\TokenMeter.ini", appdata);
    g_ini[MAX_PATH - 1] = 0;

    _snwprintf(g_logPath, MAX_PATH - 1, L"%s\\TokenMeter.log", appdata);
    g_logPath[MAX_PATH - 1] = 0;
    g_port = ini_int(L"port", DEFAULT_PORT);
    g.ctxWindow = ini_int(L"context_window", DEFAULT_WINDOW);
    g_compact = ini_int(L"compact", 0);
    g_topmost = ini_int(L"topmost", 1);
    g_warn = ini_int(L"warn", 60);
    g_danger = ini_int(L"danger", 85);
    g_cors = ini_int(L"cors", 0);
    g_refreshMin = ini_int(L"refresh_minutes", 5);
    {
        WCHAR src[16];
        GetPrivateProfileStringW(APP_NAME, L"taskbar_icon", L"max", src, 16, g_ini);
        for (int i = 0; i < 5; i++) if (!_wcsicmp(src, SRC_KEYS[i])) g_iconSrc = i;
    }
    if (g_refreshMin < 0) g_refreshMin = 0;
    WCHAR theme[16];
    GetPrivateProfileStringW(APP_NAME, L"theme", L"auto", theme, 16, g_ini);
    g_themeMode = !_wcsicmp(theme, L"dark") ? 1 : !_wcsicmp(theme, L"light") ? 2 : 0;
    if (g.ctxWindow <= 0) g.ctxWindow = DEFAULT_WINDOW;
    if (g_warn <= 0 || g_warn > 100) g_warn = 60;
    if (g_danger <= g_warn || g_danger > 100) g_danger = 85;
}

static void parse_args(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--port") && i + 1 < argc) g_port = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--window") && i + 1 < argc) g.ctxWindow = _wtoi64(argv[++i]);
        else if (!wcscmp(argv[i], L"--compact")) g_compact = 1;
        else if (!wcscmp(argv[i], L"--cors")) g_cors = 1;
    }
    LocalFree(argv);
    if (g_port <= 0 || g_port > 65535) g_port = DEFAULT_PORT;
    if (g.ctxWindow <= 0) g.ctxWindow = DEFAULT_WINDOW;
}

/* ------------------------------------------------------------------ */
/* modo --statusline: o Claude Code chama o exe com o JSON da sessão    */
/* no stdin; repassamos para a janela e imprimimos uma linha de status  */
/* ------------------------------------------------------------------ */
static int http_post_local(int port, const char *path, const char *body, size_t len, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb); /* connect com timeout: no Windows, porta fechada demora ~2 s */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(s, (struct sockaddr *)&a, sizeof a);
    fd_set wf, ef;
    FD_ZERO(&wf); FD_SET(s, &wf);
    FD_ZERO(&ef); FD_SET(s, &ef);
    struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int ok = 0;
    if (select(0, NULL, &wf, &ef, &tv) == 1 && FD_ISSET(s, &wf)) {
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD to = 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
        char hdr[256];
        int n = snprintf(hdr, sizeof hdr,
                         "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                         "Content-Length: %u\r\nConnection: close\r\n\r\n", path, (unsigned)len);
        if (send_all(s, hdr, n) && send_all(s, body, (int)len)) {
            char r[64];
            ok = recv(s, r, sizeof r, 0) > 0;
        }
    }
    closesocket(s);
    return ok;
}

static void launch_self_detached(void)
{
    WCHAR path[MAX_PATH], cmdline[MAX_PATH + 4];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    _snwprintf(cmdline, MAX_PATH + 3, L"\"%s\"", path);
    cmdline[MAX_PATH + 3] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    /* sai do "job" do Claude Code para não ser fechado junto com a status line */
    if (!CreateProcessW(path, cmdline, NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) &&
        !CreateProcessW(path, cmdline, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi))
        return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void out_utf8(HANDLE h, const WCHAR *w)
{
    char b[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, NULL, NULL);
    DWORD wr;
    if (n > 1 && h && h != INVALID_HANDLE_VALUE) WriteFile(h, b, (DWORD)(n - 1), &wr, NULL);
}

static int run_statusline(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    size_t cap = 64 * 1024, len = 0;
    char *body = (char *)malloc(cap + 1);
    if (!body) return 1;
    for (;;) {
        DWORD got = 0;
        if (len == cap) {
            if (cap >= 8 * 1024 * 1024) break;
            char *nb = (char *)realloc(body, cap * 2 + 1);
            if (!nb) break;
            body = nb;
            cap *= 2;
        }
        if (!in || in == INVALID_HANDLE_VALUE || !ReadFile(in, body + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    body[len] = 0;
    if (!len) { free(body); return 0; }

    /* 1) repassa para a janela (abre o TokenMeter se estiver fechado) */
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    if (!http_post_local(g_port, "/limits", body, len, 300)) {
        if (ini_int(L"autolaunch", 1)) {
            logmsg(LV_INFO, L"status line: TokenMeter fechado, abrindo");
            launch_self_detached();
            int ok = 0;
            for (int i = 0; i < 20 && !ok; i++) {
                Sleep(100);
                ok = http_post_local(g_port, "/limits", body, len, 200);
            }
            if (!ok) logmsg(LV_ERR, L"status line: o TokenMeter n\u00E3o respondeu na porta %d depois de abrir", g_port);
        } else {
            logmsg(LV_WARN, L"status line: TokenMeter fechado (autolaunch=0), dados descartados");
        }
    }
    WSACleanup();

    /* 2) imprime a linha de status do Claude Code */
    State st;
    memset(&st, 0, sizeof st);
    st.ctxWindow = g.ctxWindow;
    apply_limits_to(&st, body);
    free(body);

    long long now = now_epoch(), used = used_of(&st);
    WCHAR line[512], part[128], model[128], left[32];
    MultiByteToWideChar(CP_UTF8, 0, st.model, -1, model, 128);
    double ctx = st.ctxWindow > 0 ? used * 100.0 / (double)st.ctxWindow : 0;
    _snwprintf(line, 511, L"%s%scontexto %.0f%%", model, model[0] ? L" · " : L"", ctx);
    line[511] = 0;
    const Window *ws[] = { &st.fiveH, &st.sevenD, &st.spendGw };
    const WCHAR *names[] = { L"5h", L"semana", L"gasto" };
    for (int i = 0; i < 3; i++) {
        if (!ws[i]->has) continue;
        if (ws[i]->resets > now) {
            fmt_left(left, 32, ws[i]->resets - now);
            _snwprintf(part, 127, L" · %s %.0f%% (reseta em %s)", names[i], ws[i]->pct, left);
        } else {
            _snwprintf(part, 127, L" · %s liberado", names[i]);
        }
        part[127] = 0;
        if (wcslen(line) + wcslen(part) < 510) wcscat(line, part);
    }
    wcscat(line, L"\n");
    out_utf8(out, line);
    return 0;
}

#ifdef TM_DEVTOOLS
/* só para testes: grava quadros do ícone como RGBA cru (TokenMeter.exe --dump-icons pasta) */
static int dump_icons(const WCHAR *dir)
{
    set_palette();
    static const double rs[] = { 0, 0.12, 0.37, 0.64, 0.87, 1.0 };
    static const int szs[] = { 16, 32, 48 };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 6; i++)
            for (int mode = 0; mode < 3; mode++) {
                int sz = szs[k];
                DWORD *px = (DWORD *)malloc((size_t)sz * sz * 4);
                render_ring(px, sz, rs[i], status_color(rs[i]), mode == 1 ? 0.45 : 0, mode == 2 ? 0.3 : -1, sz >= 32);
                WCHAR fn[MAX_PATH];
                _snwprintf(fn, MAX_PATH - 1, L"%s\\ring_%d_%03d_%d.rgba", dir, sz, (int)(rs[i] * 100), mode);
                fn[MAX_PATH - 1] = 0;
                HANDLE f = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                DWORD w;
                if (f != INVALID_HANDLE_VALUE) { WriteFile(f, px, (DWORD)(sz * sz * 4), &w, NULL); CloseHandle(f); }
                free(px);
            }
    /* sequência animada: sobe de 12% a 64% (com brilho girando), depois a 92% e pulsa */
    {
        double shown = 0.12, target = 0.64;
        for (int fr = 0; fr < 150; fr++) {
            if (fr == 60) target = 0.92;
            double d = target - shown, step = d * 0.18;
            if (fabs(d) > 0.0005) { if (fabs(step) < 0.002) step = d > 0 ? 0.002 : -0.002; shown += step; }
            int busy = fr < 40, pulse = shown * 100 >= g_danger;
            double glow = (!busy && pulse) ? 0.45 * (0.5 - 0.5 * cos(fr * 0.25)) : 0;
            double comet = busy ? fmod(fr * 0.06, 1.0) : -1;
            DWORD px[64 * 64];
            render_ring(px, 64, shown, status_color(shown), glow, comet, 1);
            WCHAR fn[MAX_PATH];
            _snwprintf(fn, MAX_PATH - 1, L"%s\\anim_%03d.rgba", dir, fr);
            fn[MAX_PATH - 1] = 0;
            HANDLE f = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            DWORD w;
            if (f != INVALID_HANDLE_VALUE) { WriteFile(f, px, sizeof px, &w, NULL); CloseHandle(f); }
        }
    }
    return 0;
}
#endif

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
#ifdef TM_DEVTOOLS
    {
        int ac = 0;
        LPWSTR *av = CommandLineToArgvW(GetCommandLineW(), &ac);
        if (av && ac >= 3 && !wcscmp(av[1], L"--dump-icons")) return dump_icons(av[2]);
    }
#endif
    (void)hp; (void)cmd; (void)show;
    g_inst = hi;
    InitializeCriticalSection(&g_logCs);

    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int sl = 0;
        for (int i = 1; argv && i < argc; i++) if (!wcscmp(argv[i], L"--statusline")) sl = 1;
        if (argv) LocalFree(argv);
        if (sl) {
            load_settings();
            parse_args();
            return run_statusline();
        }
    }

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\TokenMeter.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"O TokenMeter já está aberto.\nProcure o ícone dele na bandeja, perto do relógio.",
                    APP_NAME, MB_ICONINFORMATION);
        return 0;
    }

    set_dpi_awareness();
    InitializeCriticalSection(&g_cs);
    load_settings();
    parse_args();

    /* log: começa o arquivo de novo se passou de 1 MB (guarda o anterior em .old) */
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 1024 * 1024)) {
            WCHAR old[MAX_PATH + 8];
            _snwprintf(old, MAX_PATH + 7, L"%s.old", g_logPath);
            old[MAX_PATH + 7] = 0;
            MoveFileExW(g_logPath, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_logReady = 1;
    logmsg(LV_INFO, L"TokenMeter " TM_VERSION_W L" iniciado \u00B7 configura\u00E7\u00F5es: %s", g_ini);

    if (!start_server()) {
        WCHAR m[512];
        _snwprintf(m, 511,
                   L"Não consegui abrir a porta %d em 127.0.0.1.\n\n"
                   L"Outro programa pode estar usando essa porta. Rode com --port <número> "
                   L"ou mude \"port\" em:\n%s", g_port, g_ini);
        m[511] = 0;
        logmsg(LV_ERR, L"Encerrando: porta %d ocupada", g_port);
        MessageBoxW(NULL, m, APP_NAME, MB_SETFOREGROUND | MB_ICONERROR);
        return 1;
    }

    read_theme();
    set_palette();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_msgTaskbarBtn = RegisterWindowMessageW(L"TaskbarButtonCreated");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.lpszClassName = L"TokenMeterWindow";
    RegisterClassExW(&wc);

    /* com botão na barra de tarefas (ícone animado), a menos que taskbar_icon=off */
    DWORD exs = WS_EX_NOACTIVATE | (g_topmost ? WS_EX_TOPMOST : 0) |
                (g_iconSrc == SRC_OFF ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW);
    g_hwnd = CreateWindowExW(exs, wc.lpszClassName, APP_NAME, WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                             0, 0, 100, 40, NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;

    g_dpi = window_dpi(g_hwnd);
    make_fonts();
    int w, h;
    layout_size(&w, &h);

    /* posição salva (se ainda estiver numa tela visível) ou canto inferior direito */
    int x = ini_int(L"x", INT_MIN), y = ini_int(L"y", INT_MIN);
    POINT probe = { x + 4, y + 4 };
    if (x == INT_MIN || y == INT_MIN || !MonitorFromPoint(probe, MONITOR_DEFAULTTONULL)) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.right - w - S(16);
        y = wa.bottom - h - S(16);
    }
    SetWindowPos(g_hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (window_dpi(g_hwnd) != g_dpi) { /* caiu num monitor com outra escala */
        g_dpi = window_dpi(g_hwnd);
        make_fonts();
        layout_size(&w, &h);
        SetWindowPos(g_hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    apply_corners();
    clamp_to_monitor();

    tips_create();
    tray_add();
    set_visible(1);
    {
        WCHAR cp[MAX_PATH];
        int ic = 0;
        logmsg(LV_INFO, L"Servidor local em http://127.0.0.1:%d", g_port);
        if (find_claude(cp, &ic)) logmsg(LV_INFO, L"Claude Code encontrado: %s", cp);
        else logmsg(LV_WARN, L"Claude Code n\u00E3o encontrado: os limites Pro/Max n\u00E3o v\u00E3o atualizar sozinhos");
    }

    g_thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    SetTimer(g_hwnd, TIMER_TICK, 500, NULL);
    if (g_refreshMin > 0) {
        SetTimer(g_hwnd, TIMER_FIRST, 1500, NULL);                          /* logo ao abrir */
        SetTimer(g_hwnd, TIMER_REFRESH, (UINT)g_refreshMin * 60000u, NULL);  /* e a cada N minutos */
    } else {
        wcscpy(g_ccStatus, L"Limites Pro/Max: consulta autom\u00E1tica desligada");
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    closesocket(g_listen); /* destrava o accept() */
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); }
    WSACleanup();
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    CoUninitialize();
    DeleteCriticalSection(&g_cs);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
