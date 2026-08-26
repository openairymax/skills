// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file plugin_code_review.c
 * @brief code_review 技能插件（C 实现，真实逻辑）
 *
 * 对代码做多维度静态审查（移植自 ecosystem/skills/src/code_review.py）：
 *   1. 正则式快速扫描（大小写不敏感）：硬编码密钥、eval()/exec()、
 *      subprocess shell=True、SQL 字符串拼接、innerHTML 赋值
 *   2. 逐行静态分析：超长行、TODO/FIXME/HACK/XXX 注释、
 *      Python 裸 except:、import *
 *   3. 去重（category+title），按 severity 排序（critical>high>medium>low>info）
 *   4. 评分：base=100，critical-25/high-15/medium-8/low-3/info-1，夹到 [0,100]
 *   5. 输出 JSON：summary / overall_score / findings[] / language / lines_reviewed
 *
 * 插件 ABI（遵循 agentrt/daemons/plugin_d 约定，见 plugin_service.h）：
 *   - plugin_get_metadata()  （必需）
 *   - plugin_init()          （必需）
 *   - plugin_destroy()       （必需）
 *   - plugin_start()         （可选：内置样例自检）
 *   - plugin_stop()          （可选）
 *   - plugin_execute()       （扩展入口：JSON 入参 → JSON 出参）
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "plugin_service.h"

#include <cjson/cJSON.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CR_MAX_FINDINGS 512   /**< findings 数组容量 */
#define CR_TITLE_LEN     96
#define CR_DESC_LEN      256
#define CR_LOC_LEN       64
#define CR_SUGGEST_LEN   128

/* ==================== 内部数据结构 ==================== */

typedef struct {
    char id[16];                /* qs-N / sa-N */
    char severity[12];          /* critical/high/medium/low/info */
    char category[20];          /* security/style/maintainability/correctness */
    char title[CR_TITLE_LEN];
    char description[CR_DESC_LEN];
    char location[CR_LOC_LEN];
    char suggestion[CR_SUGGEST_LEN];
} cr_finding_t;

typedef struct {
    cr_finding_t items[CR_MAX_FINDINGS];
    size_t count;
} cr_list_t;

/* 插件 user_data：生命周期状态 */
typedef struct {
    char config_path[256];
    char loaded_at[32];
    unsigned long execute_count;
    int running;
} cr_user_data_t;

/* ==================== 元数据 ==================== */

static const plugin_metadata_t g_metadata = {
    .name = "code_review",
    .version = "1.0.0",
    .author = "SPHARX",
    .description =
        "Multi-dimensional static code review: security pattern scanning, "
        "line-level analysis, dedup and severity scoring",
    .type = PLUGIN_TYPE_TOOL_PROVIDER,
    .api_version = 1,
    .min_airy_version = 1,
};

/* ==================== 辅助函数 ==================== */

static int cr_isspace_c(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

/* 大小写不敏感比较（UTF-8 安全：仅对 ASCII 归一化） */
static int cr_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 大小写不敏感前缀比较 */
static int cr_incmp(const char *s, const char *prefix, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!s[i]) return -1;
        int ca = tolower((unsigned char)s[i]);
        int cb = tolower((unsigned char)prefix[i]);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* 大小写不敏感子串查找（从头 / 从 from 起） */
static const char *cr_ifind(const char *text, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return text;
    for (const char *p = text; *p; p++) {
        if (cr_incmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

/* 追加一条 finding */
static void cr_add(cr_list_t *list, const char *id, const char *sev,
                   const char *cat, const char *title, const char *desc,
                   const char *loc, const char *sugg)
{
    if (list->count >= CR_MAX_FINDINGS) return;
    cr_finding_t *f = &list->items[list->count++];
    memset(f, 0, sizeof(*f));
    strncpy(f->id, id, sizeof(f->id) - 1);
    strncpy(f->severity, sev, sizeof(f->severity) - 1);
    strncpy(f->category, cat, sizeof(f->category) - 1);
    strncpy(f->title, title, sizeof(f->title) - 1);
    strncpy(f->description, desc, sizeof(f->description) - 1);
    strncpy(f->location, loc, sizeof(f->location) - 1);
    strncpy(f->suggestion, sugg, sizeof(f->suggestion) - 1);
}

/* ==================== 快速扫描（正则式，大小写不敏感） ==================== */

/* (password|passwd|secret|api_key|apikey) 空白 = 空白 "..." 硬编码赋值 */
static int cr_hardcoded_secret(const char *text)
{
    static const char *keys[] = {"password", "passwd", "secret",
                                 "api_key", "apikey"};
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = text;
        while ((p = cr_ifind(p, keys[k])) != NULL) {
            const char *q = p + strlen(keys[k]);
            while (*q && cr_isspace_c(*q)) q++;
            if (*q == '=') {
                q++;
                while (*q && cr_isspace_c(*q)) q++;
                if (*q == '"' || *q == '\'') {
                    const char *r = q + 1;
                    while (*r && *r != *q) r++;
                    if (r > q + 1 && *r == *q) return 1;
                }
            }
            p = q;
        }
    }
    return 0;
}

/* eval( / exec( 调用 */
static int cr_call_paren(const char *text, const char *kw)
{
    const char *p = text;
    while ((p = cr_ifind(p, kw)) != NULL) {
        const char *q = p + strlen(kw);
        while (*q && cr_isspace_c(*q)) q++;
        if (*q == '(') return 1;
        p = q;
    }
    return 0;
}

/* shell 空白 = 空白 True */
static int cr_shell_true(const char *text)
{
    const char *p = text;
    while ((p = cr_ifind(p, "shell")) != NULL) {
        const char *q = p + 5;
        while (*q && cr_isspace_c(*q)) q++;
        if (*q == '=') {
            q++;
            while (*q && cr_isspace_c(*q)) q++;
            if (cr_incmp(q, "true", 4) == 0 &&
                !isalnum((unsigned char)q[4]))
                return 1;
        }
        p = q;
    }
    return 0;
}

/* SELECT ... + ... FROM （SQL 字符串拼接） */
static int cr_sql_concat(const char *text)
{
    const char *s = cr_ifind(text, "select");
    while (s) {
        const char *plus = cr_ifind(s + 6, "+");
        while (plus) {
            if (cr_ifind(plus + 1, "from"))
                return 1;
            plus = cr_ifind(plus + 1, "+");
        }
        s = cr_ifind(s + 6, "select");
    }
    return 0;
}

/* innerHTML 空白 = */
static int cr_innerhtml(const char *text)
{
    const char *p = text;
    while ((p = cr_ifind(p, "innerhtml")) != NULL) {
        const char *q = p + 9;
        while (*q && cr_isspace_c(*q)) q++;
        if (*q == '=') return 1;
        p = q;
    }
    return 0;
}

static void cr_quick_scan(const char *code, cr_list_t *list)
{
    size_t n = list->count;
    if (cr_hardcoded_secret(code)) {
        cr_add(list, "qs-1", "critical", "security",
               "Hardcoded secret or API key",
               "Pattern detected: Hardcoded secret or API key",
               "quick-scan", "Review and remediate");
    }
    if (cr_call_paren(code, "eval")) {
        cr_add(list, "qs-2", "high", "security",
               "Use of eval() — potential code injection",
               "Pattern detected: Use of eval() — potential code injection",
               "quick-scan", "Review and remediate");
    }
    if (cr_call_paren(code, "exec")) {
        cr_add(list, "qs-3", "high", "security",
               "Use of exec() — potential code injection",
               "Pattern detected: Use of exec() — potential code injection",
               "quick-scan", "Review and remediate");
    }
    if (cr_shell_true(code)) {
        cr_add(list, "qs-4", "high", "security",
               "Shell injection risk with subprocess(shell=True)",
               "Pattern detected: Shell injection risk with subprocess(shell=True)",
               "quick-scan", "Review and remediate");
    }
    if (cr_sql_concat(code)) {
        cr_add(list, "qs-5", "high", "security",
               "Potential SQL injection via string concatenation",
               "Pattern detected: Potential SQL injection via string concatenation",
               "quick-scan", "Review and remediate");
    }
    if (cr_innerhtml(code)) {
        cr_add(list, "qs-6", "medium", "security",
               "XSS risk with innerHTML assignment",
               "Pattern detected: XSS risk with innerHTML assignment",
               "quick-scan", "Review and remediate");
    }
}

/* ==================== 逐行静态分析 ==================== */

static void cr_static_analysis(const char *code, const char *language,
                               cr_list_t *list)
{
    static const char *markers[] = {"TODO", "FIXME", "HACK", "XXX"};
    size_t sa_no = 0;
    const char *line_start = code;
    const char *p = code;
    int lineno = 1;

    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* 去除首尾空白 */
        const char *s = p;
        size_t l = len;
        while (l && cr_isspace_c(*s)) { s++; l--; }
        while (l && cr_isspace_c(s[l - 1])) l--;

        /* 超长行（>120 字符） */
        if (l > 120) {
            char id[16], desc[CR_DESC_LEN], loc[CR_LOC_LEN];
            snprintf(id, sizeof(id), "sa-%zu", ++sa_no);
            snprintf(desc, sizeof(desc),
                     "Line %d exceeds 120 characters (%zu chars)", lineno, l);
            snprintf(loc, sizeof(loc), "line %d", lineno);
            cr_add(list, id, "info", "style", "Line too long", desc, loc,
                   "Break into multiple lines");
        }

        /* TODO/FIXME/HACK/XXX 注释（大小写不敏感） */
        for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
            if (cr_ifind(s, markers[m])) {
                char id[16], desc[CR_DESC_LEN], loc[CR_LOC_LEN],
                     title[CR_TITLE_LEN];
                snprintf(id, sizeof(id), "sa-%zu", ++sa_no);
                snprintf(title, sizeof(title), "%s comment found",
                         markers[m]);
                snprintf(desc, sizeof(desc), "Line %d contains %s",
                         lineno, markers[m]);
                snprintf(loc, sizeof(loc), "line %d", lineno);
                cr_add(list, id, "info", "maintainability", title, desc,
                       loc, "Resolve or create tracking issue");
            }
        }

        /* Python 特定检查 */
        if (cr_icmp(language, "python") == 0) {
            if (strstr(s, "except:") && !strstr(s, "except Exception")) {
                char id[16], desc[CR_DESC_LEN], loc[CR_LOC_LEN];
                snprintf(id, sizeof(id), "sa-%zu", ++sa_no);
                snprintf(desc, sizeof(desc),
                         "Line %d: bare 'except:' catches all exceptions "
                         "including KeyboardInterrupt", lineno);
                snprintf(loc, sizeof(loc), "line %d", lineno);
                cr_add(list, id, "medium", "correctness",
                       "Bare except clause", desc, loc,
                       "Use 'except Exception:' or specify exception types");
            }
            if (strstr(s, "import *")) {
                char id[16], desc[CR_DESC_LEN], loc[CR_LOC_LEN];
                snprintf(id, sizeof(id), "sa-%zu", ++sa_no);
                snprintf(desc, sizeof(desc),
                         "Line %d: 'import *' pollutes namespace", lineno);
                snprintf(loc, sizeof(loc), "line %d", lineno);
                cr_add(list, id, "low", "style", "Wildcard import", desc,
                       loc, "Import specific names");
            }
        }

        if (!eol) break;
        p = eol + 1;
        lineno++;
    }
    (void)line_start;
}

/* ==================== 去重 / 排序 / 过滤 / 评分 ==================== */

static int cr_sev_rank(const char *sev)
{
    if (strcmp(sev, "critical") == 0) return 0;
    if (strcmp(sev, "high") == 0) return 1;
    if (strcmp(sev, "medium") == 0) return 2;
    if (strcmp(sev, "low") == 0) return 3;
    if (strcmp(sev, "info") == 0) return 4;
    return 5;
}

/* 去重：键 = category + '\x01' + title */
static void cr_dedup(cr_list_t *list)
{
    size_t w = 0;
    for (size_t i = 0; i < list->count; i++) {
        int dup = 0;
        for (size_t j = 0; j < w; j++) {
            if (strcmp(list->items[i].category, list->items[j].category) == 0 &&
                strcmp(list->items[i].title, list->items[j].title) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (w != i) list->items[w] = list->items[i];
            w++;
        }
    }
    list->count = w;
}

/* 稳定插入排序：severity 升序（critical 最前） */
static void cr_sort(cr_list_t *list)
{
    for (size_t i = 1; i < list->count; i++) {
        cr_finding_t key = list->items[i];
        size_t j = i;
        while (j > 0 &&
               cr_sev_rank(list->items[j - 1].severity) >
                   cr_sev_rank(key.severity)) {
            list->items[j] = list->items[j - 1];
            j--;
        }
        list->items[j] = key;
    }
}

/* 阈值过滤：info=0 < low=1 < medium=2 < high=3 < critical=4 */
static void cr_filter(cr_list_t *list, const char *threshold)
{
    int min_level = 1; /* 默认 low */
    if (cr_icmp(threshold, "info") == 0) min_level = 0;
    else if (cr_icmp(threshold, "medium") == 0) min_level = 2;
    else if (cr_icmp(threshold, "high") == 0) min_level = 3;
    else if (cr_icmp(threshold, "critical") == 0) min_level = 4;

    size_t w = 0;
    for (size_t i = 0; i < list->count; i++) {
        int lvl = cr_sev_rank(list->items[i].severity);
        if (lvl >= 5) lvl = 4; /* 未知按 info 级算 */
        if (4 - lvl >= min_level) { /* severity 级别数 >= 阈值 */
            if (w != i) list->items[w] = list->items[i];
            w++;
        }
    }
    list->count = w;
}

static double cr_compute_score(const cr_list_t *list)
{
    double base = 100.0;
    for (size_t i = 0; i < list->count; i++) {
        const char *sev = list->items[i].severity;
        if (strcmp(sev, "critical") == 0) base -= 25;
        else if (strcmp(sev, "high") == 0) base -= 15;
        else if (strcmp(sev, "medium") == 0) base -= 8;
        else if (strcmp(sev, "low") == 0) base -= 3;
        else base -= 1;
    }
    if (base < 0.0) base = 0.0;
    if (base > 100.0) base = 100.0;
    return base;
}

static void cr_make_summary(char *buf, size_t bufsz, const char *language,
                            const cr_list_t *list, double score)
{
    unsigned counts[5] = {0, 0, 0, 0, 0};
    static const char *names[5] = {"critical", "high", "medium",
                                   "low", "info"};
    for (size_t i = 0; i < list->count; i++) {
        int r = cr_sev_rank(list->items[i].severity);
        if (r >= 0 && r < 5) counts[r]++;
    }
    buf[0] = '\0';
    size_t n = snprintf(buf, bufsz, "Code review (%s): score %.0f/100",
                        language, score);
    for (int i = 0; i < 5; i++) {
        if (counts[i] > 0 && n < bufsz) {
            n += snprintf(buf + n, bufsz - n, ", %u %s", counts[i], names[i]);
        }
    }
}

/* 行数统计（splitlines 近似：'\n' 计数 + 1） */
static size_t cr_count_lines(const char *text)
{
    if (!text || !*text) return 0;
    size_t n = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') n++;
    }
    return n;
}

/* ==================== 插件 ABI 实现 ==================== */

const plugin_metadata_t *plugin_get_metadata(void)
{
    return &g_metadata;
}

int plugin_init(const char *config_path, void **user_data)
{
    if (!user_data) return -1;

    cr_user_data_t *ud = (cr_user_data_t *)calloc(1, sizeof(cr_user_data_t));
    if (!ud) return -1;

    if (config_path) {
        strncpy(ud->config_path, config_path, sizeof(ud->config_path) - 1);
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ud->loaded_at, sizeof(ud->loaded_at), "%Y-%m-%d %H:%M:%S", &tmv);
    ud->running = 0;

    *user_data = ud;
    return 0;
}

int plugin_start(void *user_data)
{
    cr_user_data_t *ud = (cr_user_data_t *)user_data;
    if (!ud) return -1;

    /* 自检：对内置样例执行一次真实审查，验证核心逻辑可用 */
    static const char sample[] =
        "password = \"hunter2\"\n"
        "result = eval(user_input)\n"
        "TODO: refactor this function\n";

    cr_list_t list;
    memset(&list, 0, sizeof(list));
    cr_quick_scan(sample, &list);
    cr_static_analysis(sample, "python", &list);
    if (list.count == 0) return -1;

    ud->running = 1;
    return 0;
}

int plugin_stop(void *user_data)
{
    cr_user_data_t *ud = (cr_user_data_t *)user_data;
    if (!ud) return -1;
    ud->running = 0;
    return 0;
}

void plugin_destroy(void *user_data)
{
    free(user_data);
}

/**
 * @brief 技能执行入口（plugin.execute RPC 调用）
 *
 * 输入 JSON：{"code": "...", "language": "python",
 *             "focus": "all", "severity_threshold": "low"}
 * 输出 JSON：{"ok": true, "summary": "...", "overall_score": N,
 *             "findings": [...], "language": "...", "focus": "...",
 *             "lines_reviewed": N}
 *
 * @param[in]  json_input  输入 JSON 字符串
 * @param[out] json_output 输出 JSON 字符串（cJSON malloc，由 plugin_d
 *                         通过 AIRY_FREE 释放；POSIX 下即 free）
 * @return 0 成功，非 0 失败
 */
int plugin_execute(const char *json_input, char **json_output)
{
    if (!json_input || !json_output) return -1;
    *json_output = NULL;

    cJSON *root = cJSON_Parse(json_input);
    if (!root) return -1;

    cJSON *code_item = cJSON_GetObjectItem(root, "code");
    cJSON *lang_item = cJSON_GetObjectItem(root, "language");
    if (!cJSON_IsString(code_item) || !cJSON_IsString(lang_item)) {
        cJSON_Delete(root);
        return -1;
    }

    const char *code = code_item->valuestring;
    const char *s = code;
    while (*s && cr_isspace_c(*s)) s++;
    if (!*s) { /* 空白代码视为空输入 */
        cJSON_Delete(root);
        return -1;
    }

    const char *language = lang_item->valuestring;
    static const char *supported[] = {"python", "rust", "javascript",
                                      "go", "c", "java", "typescript", "cpp"};
    int lang_ok = 0;
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (cr_icmp(language, supported[i]) == 0) { lang_ok = 1; break; }
    }
    if (!lang_ok) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *focus_item = cJSON_GetObjectItem(root, "focus");
    const char *focus = cJSON_IsString(focus_item) ? focus_item->valuestring
                                                   : "all";

    cJSON *thr_item = cJSON_GetObjectItem(root, "severity_threshold");
    const char *threshold = cJSON_IsString(thr_item) &&
                                    thr_item->valuestring[0]
                                ? thr_item->valuestring
                                : "low";

    /* 1) 收集全部 findings */
    cr_list_t list;
    memset(&list, 0, sizeof(list));
    cr_quick_scan(code, &list);
    cr_static_analysis(code, language, &list);

    /* 2) 去重 → 排序 → 阈值过滤 */
    cr_dedup(&list);
    cr_sort(&list);
    cr_filter(&list, threshold);

    /* 3) 评分与摘要 */
    double score = cr_compute_score(&list);
    char summary[512];
    cr_make_summary(summary, sizeof(summary), language, &list, score);
    size_t lines = cr_count_lines(code);

    /* 4) 输出 JSON */
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "summary", summary);
    cJSON_AddNumberToObject(out, "overall_score", score);
    cJSON_AddStringToObject(out, "language", language);
    cJSON_AddStringToObject(out, "focus", focus);
    cJSON_AddNumberToObject(out, "lines_reviewed", (double)lines);

    cJSON *arr = cJSON_AddArrayToObject(out, "findings");
    for (size_t i = 0; i < list.count; i++) {
        cr_finding_t *f = &list.items[i];
        cJSON *fj = cJSON_CreateObject();
        if (!fj) continue;
        cJSON_AddStringToObject(fj, "id", f->id);
        cJSON_AddStringToObject(fj, "severity", f->severity);
        cJSON_AddStringToObject(fj, "category", f->category);
        cJSON_AddStringToObject(fj, "title", f->title);
        cJSON_AddStringToObject(fj, "description", f->description);
        cJSON_AddStringToObject(fj, "location", f->location);
        cJSON_AddStringToObject(fj, "suggestion", f->suggestion);
        cJSON_AddItemToArray(arr, fj);
    }

    char *serialized = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!serialized) return -1;

    *json_output = serialized;
    return 0;
}
