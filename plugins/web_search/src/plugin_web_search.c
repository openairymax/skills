// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file plugin_web_search.c
 * @brief web_search 技能插件（C 实现，真实逻辑）
 *
 * 网络搜索（真实联网 + fallback 语义移植自 ecosystem/skills/src/web_search.py）：
 * C 插件经 fork+exec curl 抓取 DuckDuckGo HTML 结果页（可配置端点
 * AIRY_PLUGIN_WS_ENDPOINT），解析 result__a/result__snippet 产出真实结果；
 * 网络不可用/无 curl/超时/反爬时降级为搜索引擎链接生成，并完整实现其
 * 结果处理管线：
 *   1. 搜索引擎链接生成：query 做 URL 编码（空格→%20 等非字母数字字符），
 *      duckduckgo（relevance 1.0）与 google（relevance 0.9）两个结果
 *   2. URL 规范化去重：lowercase、去尾部 /、去 https?://www. 前缀、
 *      去 utm_*、ref、source 参数
 *   3. 相关性排序：query 词在 title+snippet 的匹配率
 *      （0.6×relevance + 0.4×keyword_score），权威域加分，降序
 *   4. 截断到 max_results（默认 10，clamp 1..50）；query 长度>500 拒绝
 *   5. 输出 JSON：query / results[] / total_found / summary / mode / engine
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
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WS_MAX_QUERY    500      /**< query 最大长度 */
#define WS_MAX_RESULTS  50       /**< 结果上限 */
#define WS_MAX_TERMS    256      /**< query 拆分词数上限 */
#define WS_TITLE_LEN    512      /**< 标题缓冲区 */
#define WS_URL_LEN      1600     /**< URL 缓冲区（容纳全量编码 query） */
#define WS_SNIPPET_LEN  1024     /**< 摘要缓冲区 */
#define WS_SOURCE_LEN   64       /**< 引擎名缓冲区 */

/* ==================== 内部数据结构 ==================== */

typedef struct {
    char title[WS_TITLE_LEN];
    char url[WS_URL_LEN];
    char snippet[WS_SNIPPET_LEN];
    char source[WS_SOURCE_LEN];
    double relevance;        /* 初始基础分，排序后被最终分覆盖 */
} ws_result_t;

typedef struct {
    ws_result_t items[WS_MAX_RESULTS];
    size_t count;
} ws_result_set_t;

/* 权威域加分表（对齐 Python 实现） */
typedef struct {
    const char *domain;
    double boost;
} ws_authority_t;

static const ws_authority_t k_authorities[] = {
    {"github.com", 1.2},
    {"docs.rs", 1.3},
    {"python.org", 1.3},
    {"rust-lang.org", 1.3},
    {"arxiv.org", 1.2},
    {"stackoverflow.com", 1.1},
    {"wikipedia.org", 1.1},
    {"mozilla.org", 1.2},
};

/* 插件 user_data：生命周期状态 */
typedef struct {
    char config_path[256];
    char loaded_at[32];
    unsigned long execute_count;
    int running;
} ws_user_data_t;

/* ==================== 元数据 ==================== */

static const plugin_metadata_t g_metadata = {
    .name = "web_search",
    .version = "1.0.0",
    .author = "SPHARX",
    .description =
        "Web search fallback provider: search engine link generation, "
        "URL normalization, keyword-based relevance ranking",
    .type = PLUGIN_TYPE_TOOL_PROVIDER,
    .api_version = 1,
    .min_airy_version = 1,
};

/* ==================== 辅助函数 ==================== */

/** 生成带指定位数小数的 JSON 原始数字节点（保证输出位数精确） */
static cJSON *ws_raw_num(double v, int digits)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return cJSON_CreateRaw(buf);
}

/**
 * @brief 真实 URL 百分号编码（UTF-8 逐字节处理）
 * 字母数字与 - _ . ~ 保留，其余字符（空格→%20、&→%26、?→%3F、
 * =→%3D、#→%23、/→%2F 等）编码为大写十六进制 %XX。
 */
static int ws_url_encode(const char *src, char *dst, size_t cap)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (j + 1 >= cap)
                return -1;
            dst[j++] = (char)c;
        } else {
            if (j + 3 >= cap)
                return -1;
            dst[j++] = '%';
            dst[j++] = hexd[c >> 4];
            dst[j++] = hexd[c & 0x0F];
        }
    }
    dst[j] = '\0';
    return 0;
}

/**
 * @brief URL 百分号解码（%XX → 字节，+ → 空格）
 * @return 0 成功；非法 % 序列跳过原样保留
 */
static int ws_url_decode(const char *src, char *dst, size_t cap)
{
    if (!src || !dst || cap == 0)
        return -1;
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p == '+' && j + 1 < cap) {
            dst[j++] = ' ';
        } else if (*p == '%' && p[1] && p[2] &&
                   isxdigit(p[1]) && isxdigit(p[2])) {
            if (j + 1 >= cap)
                return -1;
            unsigned int hi = (unsigned int)((p[1] <= '9') ? p[1] - '0'
                        : (tolower(p[1]) - 'a' + 10));
            unsigned int lo = (unsigned int)((p[2] <= '9') ? p[2] - '0'
                        : (tolower(p[2]) - 'a' + 10));
            dst[j++] = (char)((hi << 4) | lo);
            p += 2;
        } else {
            if (j + 1 >= cap)
                return -1;
            dst[j++] = (char)*p;
        }
    }
    dst[j] = '\0';
    return 0;
}

/**
 * @brief URL 规范化（去重用）：lowercase、去尾部 /、去 https?://www.
 * 前缀、去 utm_*、ref、source 参数（对齐 Python re.sub 语义）
 */
static void ws_normalize_url(const char *url, char *out, size_t cap)
{
    char buf[WS_URL_LEN * 2];
    size_t i = 0, j = 0;
    while (url[i] && j + 1 < sizeof(buf)) {
        buf[j++] = (char)tolower((unsigned char)url[i++]);
    }
    buf[j] = '\0';

    /* 去尾部 '/' */
    while (j > 0 && buf[j - 1] == '/')
        buf[--j] = '\0';

    /* 去 ^https?://(www\.)? 前缀 */
    if (strncmp(buf, "https://", 8) == 0)
        memmove(buf, buf + 8, strlen(buf + 8) + 1);
    else if (strncmp(buf, "http://", 7) == 0)
        memmove(buf, buf + 7, strlen(buf + 7) + 1);
    if (strncmp(buf, "www.", 4) == 0)
        memmove(buf, buf + 4, strlen(buf + 4) + 1);

    /* 去参数：'?' 或 '&' 后参数名为 utm_* / ref / source 时整段删除 */
    size_t o = 0;
    i = 0;
    while (buf[i]) {
        if (buf[i] == '?' || buf[i] == '&') {
            size_t name_start = i + 1;
            size_t p = name_start;
            while (buf[p] && buf[p] != '=' && buf[p] != '&')
                p++;
            size_t name_len = p - name_start;
            int drop = 0;
            if (name_len >= 4 && strncmp(buf + name_start, "utm_", 4) == 0)
                drop = 1;
            else if (name_len == 3 && strncmp(buf + name_start, "ref", 3) == 0)
                drop = 1;
            else if (name_len == 6 &&
                     strncmp(buf + name_start, "source", 6) == 0)
                drop = 1;
            if (drop) {
                i = p; /* 跳过参数名 */
                if (buf[i] == '=') {
                    i++;
                    while (buf[i] && buf[i] != '&')
                        i++;
                }
                continue; /* 不复制 '?'/'&' 与参数 */
            }
        }
        if (o + 1 < cap)
            out[o++] = buf[i];
        i++;
    }
    out[o] = '\0';
}

/* ==================== 结果管线 ==================== */

/** 追加一条搜索结果（上限 WS_MAX_RESULTS） */
static void ws_add_result(ws_result_set_t *set, const char *query,
                          const char *url, const char *snippet,
                          const char *source, double relevance)
{
    if (set->count >= WS_MAX_RESULTS)
        return;
    ws_result_t *r = &set->items[set->count];
    snprintf(r->title, sizeof(r->title), "Search: %s", query);
    snprintf(r->url, sizeof(r->url), "%s", url);
    snprintf(r->snippet, sizeof(r->snippet), "%s", snippet);
    snprintf(r->source, sizeof(r->source), "%s", source);
    r->relevance = relevance;
    set->count++;
}

/** 基于 URL 规范化去重（保序） */
static void ws_deduplicate(ws_result_set_t *set)
{
    for (size_t i = 0; i < set->count; i++) {
        char norm[WS_URL_LEN * 2];
        ws_normalize_url(set->items[i].url, norm, sizeof(norm));
        for (size_t j = i + 1; j < set->count; j++) {
            char norm2[WS_URL_LEN * 2];
            ws_normalize_url(set->items[j].url, norm2, sizeof(norm2));
            if (strcmp(norm, norm2) == 0) {
                memmove(&set->items[j], &set->items[j + 1],
                        (set->count - j - 1) * sizeof(ws_result_t));
                set->count--;
                j--;
            }
        }
    }
}

/** 关键词匹配率：query 词在 title+snippet（lowercase）中的子串匹配比例 */
static double ws_keyword_score(const char *title, const char *snippet,
                               const char *query)
{
    char qbuf[WS_MAX_QUERY + 1];
    size_t ql = strlen(query);
    if (ql > WS_MAX_QUERY)
        ql = WS_MAX_QUERY;
    memcpy(qbuf, query, ql);
    qbuf[ql] = '\0';
    for (size_t i = 0; i < ql; i++)
        qbuf[i] = (char)tolower((unsigned char)qbuf[i]);

    /* 拼接 title + snippet（lowercase） */
    char text[WS_TITLE_LEN + WS_SNIPPET_LEN + 2];
    size_t tl = 0;
    for (const char *p = title; *p && tl + 1 < sizeof(text); p++)
        text[tl++] = (char)tolower((unsigned char)*p);
    text[tl++] = ' ';
    for (const char *p = snippet; *p && tl + 1 < sizeof(text); p++)
        text[tl++] = (char)tolower((unsigned char)*p);
    text[tl] = '\0';

    size_t nterms = 0, matched = 0;
    const char *p = qbuf;
    while (*p && nterms < WS_MAX_TERMS) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len > 0) {
            nterms++;
            char term[WS_MAX_QUERY + 1];
            if (len > WS_MAX_QUERY)
                len = WS_MAX_QUERY;
            memcpy(term, start, len);
            term[len] = '\0';
            if (strstr(text, term))
                matched++;
        }
    }
    if (nterms == 0)
        return 0.0;
    return (double)matched / (double)nterms;
}

/** 相关性排序：0.6×relevance + 0.4×keyword_score，权威域加分，降序 */
static void ws_rank(ws_result_set_t *set, const char *query)
{
    for (size_t i = 0; i < set->count; i++) {
        ws_result_t *r = &set->items[i];
        double kw = ws_keyword_score(r->title, r->snippet, query);
        double score = r->relevance * 0.6 + kw * 0.4;

        char url_lower[WS_URL_LEN];
        size_t u = 0;
        for (const char *p = r->url; *p && u + 1 < sizeof(url_lower); p++)
            url_lower[u++] = (char)tolower((unsigned char)*p);
        url_lower[u] = '\0';
        for (size_t a = 0;
             a < sizeof(k_authorities) / sizeof(k_authorities[0]); a++) {
            if (strstr(url_lower, k_authorities[a].domain)) {
                score *= k_authorities[a].boost;
                break;
            }
        }
        r->relevance = score;
    }

    /* 插入排序：按 relevance 降序 */
    for (size_t i = 1; i < set->count; i++) {
        ws_result_t key = set->items[i];
        size_t j = i;
        while (j > 0 && set->items[j - 1].relevance < key.relevance) {
            set->items[j] = set->items[j - 1];
            j--;
        }
        set->items[j] = key;
    }
}

/* ==================== 真实联网抓取（curl 子进程，失败降级） ==================== */

#define WS_HTTP_TIMEOUT_MS 8000  /**< 抓取超时 */
#define WS_HTTP_MAX_BYTES (1 << 20) /**< 响应上限 1MB */

/**
 * @brief 经 fork+exec curl 抓取 URL（POSIX only）
 *
 * 非阻塞读取 + 总超时；成功返回 0 且 out 为 null 结尾的响应体
 * （截断到 WS_HTTP_MAX_BYTES）。curl 缺失/超时/非零退出均返回 -1。
 */
static int ws_http_get(const char *url, char *out, size_t cap)
{
    if (!url || !out || cap < 64)
        return -1;

    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程：stdout 接管道，exec curl */
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp("curl", "curl", "-sL", "--max-time", "7", "-A",
               "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36",
               url, (char *)NULL);
        _exit(127); /* exec 失败 */
    }

    close(fds[1]);

    /* 父进程：poll 非阻塞读直到超时/EOF */
    size_t used = 0;
    int timed_out = 0;
    long elapsed = 0;
    for (;;) {
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            elapsed += 200;
            if (elapsed >= WS_HTTP_TIMEOUT_MS) {
                timed_out = 1;
                break;
            }
            continue;
        }
        ssize_t n = read(fds[0], out + used, cap - used - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used >= cap - 1) {
            out[used] = '\0';
            break;
        }
    }
    close(fds[0]);
    out[used] = '\0';

    int status = 0;
    if (!timed_out)
        waitpid(pid, &status, 0);
    else
        waitpid(pid, &status, WNOHANG);

    if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return (used > 0) ? 0 : -1;
}

/** HTML 实体解码（&amp; &lt; &gt; &quot; &#39; 等常见实体） */
static void ws_html_unescape(char *s)
{
    const char *src = s;
    char *dst = s;
    while (*src) {
        if (src[0] == '&') {
            if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
                continue;
            }
            if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
                continue;
            }
            if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
                continue;
            }
            if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '"';
                src += 6;
                continue;
            }
            if (strncmp(src, "&#39;", 5) == 0) {
                *dst++ = '\'';
                src += 5;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/** 提取首个 class 属性中带指定类的 a 标签：返回 href 与文本（剥离标签） */
static int ws_extract_anchor(const char *html, const char *class_name,
                             char *href, size_t href_cap,
                             char *text, size_t text_cap)
{
    const char *needle = class_name;
    size_t nlen = strlen(needle);
    const char *p = strstr(html, needle);
    if (!p)
        return -1;

    /* 回退到最近的前一个 '<a ' 找 href */
    const char *tag = p;
    while (tag > html && tag[-1] != '<')
        tag--;
    const char *h = strstr(tag, "href=");
    if (h) {
        h += 5;
        if (*h == '"' || *h == '\'') {
            char q = *h++;
            size_t k = 0;
            while (*h && *h != q && k + 1 < href_cap)
                href[k++] = *h++;
            href[k] = '\0';
        }
    }

    /* 提取标签文本：跳到 '>' 后，直到 '<' */
    const char *gt = strchr(p, '>');
    if (gt) {
        const char *t = gt + 1;
        size_t k = 0;
        while (*t && *t != '<' && k + 1 < text_cap)
            text[k++] = *t++;
        text[k] = '\0';
        ws_html_unescape(text);
    }
    return (href[0] && text[0]) ? 0 : -1;
}

/**
 * @brief 解析 DuckDuckGo HTML 结果页（result__a / result__snippet 结构）
 *
 * 逐个提取 ``<a class="result__a" ...>title</a>`` 与
 * ``<a class="result__snippet" ...>snippet</a>``；``result__a`` 的
 * href 经 DDG 重定向去除（uddg/uddg 参数）。解析不足 1 条返回 -1。
 */
static int ws_parse_ddg_html(const char *html, const char *query,
                             ws_result_set_t *set, size_t max_results)
{
    if (!html || !query || !set)
        return -1;

    const char *cursor = html;
    size_t added = 0;
    while (added < max_results && added < WS_MAX_RESULTS) {
        char href[WS_URL_LEN];
        char title[WS_TITLE_LEN];
        char snip[WS_SNIPPET_LEN];
        href[0] = title[0] = snip[0] = '\0';

        const char *next = strstr(cursor, "result__a");
        if (!next)
            break;
        if (ws_extract_anchor(cursor, "result__a", href, sizeof(href),
                              title, sizeof(title)) != 0)
            break;

        /* 截取 snippet 段（在本次 result__a 之后寻找最近的 result__snippet） */
        snip[0] = '\0';
        const char *snip_seg = strstr(next, "result__snippet");
        const char *next_result = strstr(next + 1, "result__a");
        if (snip_seg && (!next_result || snip_seg < next_result)) {
            char shref[64];
            shref[0] = '\0';
            ws_extract_anchor(snip_seg, "result__snippet", shref,
                              sizeof(shref), snip, sizeof(snip));
        }

        /* 去除 DDG 跳转参数（uddg=<编码URL>） */
        char real_url[WS_URL_LEN];
        real_url[0] = '\0';
        const char *uddg = strstr(href, "uddg=");
        if (uddg) {
            uddg += 5;
            size_t k = 0;
            while (*uddg && *uddg != '&' && k + 1 < sizeof(real_url))
                real_url[k++] = *uddg++;
            real_url[k] = '\0';
            ws_url_decode(real_url, href, sizeof(href));
        }

        ws_add_result(set, query, href[0] ? href : "https://duckduckgo.com/",
                      snip[0] ? snip : "DuckDuckGo result", "duckduckgo", 1.0);
        added++;
        cursor = next + 1;
    }
    return (added > 0) ? 0 : -1;
}

/* ==================== 插件 ABI 实现 ==================== */

const plugin_metadata_t *plugin_get_metadata(void)
{
    return &g_metadata;
}

int plugin_init(const char *config_path, void **user_data)
{
    if (!user_data)
        return -1;

    ws_user_data_t *ud = (ws_user_data_t *)calloc(1, sizeof(ws_user_data_t));
    if (!ud)
        return -1;

    if (config_path)
        strncpy(ud->config_path, config_path, sizeof(ud->config_path) - 1);
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
    ws_user_data_t *ud = (ws_user_data_t *)user_data;
    if (!ud)
        return -1;

    /* 自检：URL 编码 + 链接生成管线可用 */
    char encoded[WS_MAX_QUERY * 3 + 1];
    if (ws_url_encode("rust async", encoded, sizeof(encoded)) != 0)
        return -1;
    if (strcmp(encoded, "rust%20async") != 0)
        return -1;

    ws_result_set_t set;
    memset(&set, 0, sizeof(set));
    ws_add_result(&set, "rust async", "https://duckduckgo.com/?q=rust%20async",
                  "DuckDuckGo search results for: rust async", "duckduckgo",
                  1.0);
    ws_add_result(&set, "rust async",
                  "https://www.google.com/search?q=rust%20async",
                  "Google search results for: rust async", "google", 0.9);
    ws_deduplicate(&set);
    ws_rank(&set, "rust async");
    if (set.count != 2 || set.items[0].relevance < set.items[1].relevance)
        return -1;

    ud->running = 1;
    return 0;
}

int plugin_stop(void *user_data)
{
    ws_user_data_t *ud = (ws_user_data_t *)user_data;
    if (!ud)
        return -1;
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
 * 输入 JSON：{"query": "...", "max_results": 10, "engine": "auto",
 *             "time_range": "all", "region": "us"}
 * 输出 JSON：{"query": "...", "results": [{"title","url","snippet",
 *             "source","relevance"}], "total_found": N,
 *             "summary": "...", "engine": "..."}
 *
 * @param[in]  json_input  输入 JSON 字符串
 * @param[out] json_output 输出 JSON 字符串（cJSON malloc，由 plugin_d
 *                         通过 AIRY_FREE 释放；POSIX 下即 free）
 * @return 0 成功，非 0 失败（输入为空/字段缺失返回 -1）
 */
int plugin_execute(const char *json_input, char **json_output)
{
    if (!json_input || !json_output)
        return -1;
    *json_output = NULL;

    cJSON *root = cJSON_Parse(json_input);
    if (!root)
        return -1;

    cJSON *q = cJSON_GetObjectItem(root, "query");
    if (!cJSON_IsString(q) || q->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }
    if (strlen(q->valuestring) > WS_MAX_QUERY) {
        /* query 长度 >500 拒绝 */
        cJSON_Delete(root);
        return -1;
    }

    long max_results = 10;
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    if (cJSON_IsNumber(mr))
        max_results = (long)mr->valuedouble;
    if (max_results < 1)
        max_results = 1;
    if (max_results > WS_MAX_RESULTS)
        max_results = WS_MAX_RESULTS;

    cJSON *eng = cJSON_GetObjectItem(root, "engine");
    const char *engine = cJSON_IsString(eng) ? eng->valuestring : "auto";
    cJSON *tr = cJSON_GetObjectItem(root, "time_range");
    const char *time_range = cJSON_IsString(tr) ? tr->valuestring : "all";

    /* URL 编码 query */
    char encoded[WS_MAX_QUERY * 3 + 1];
    if (ws_url_encode(q->valuestring, encoded, sizeof(encoded)) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    /* 真实联网：抓取 DuckDuckGo HTML 结果页（可配置端点，默认 DDG）。
     * 抓取/解析失败（无网络、无 curl、超时、反爬）→ 降级链接生成。 */
    ws_result_set_t set;
    memset(&set, 0, sizeof(set));
    int online = 0;
    const char *endpoint = getenv("AIRY_PLUGIN_WS_ENDPOINT");
    char ddg_url[WS_URL_LEN + 64];
    snprintf(ddg_url, sizeof(ddg_url), "%s",
             (endpoint && endpoint[0]) ? endpoint
                                       : "https://html.duckduckgo.com/html/");
    char full_url[WS_URL_LEN + 64];
    if (strchr(ddg_url, '?') != NULL)
        snprintf(full_url, sizeof(full_url), "%s&q=%s", ddg_url, encoded);
    else
        snprintf(full_url, sizeof(full_url), "%s?q=%s", ddg_url, encoded);

    char *html_buf = (char *)malloc(WS_HTTP_MAX_BYTES + 1);
    if (html_buf) {
        if (ws_http_get(full_url, html_buf, WS_HTTP_MAX_BYTES + 1) == 0 &&
            ws_parse_ddg_html(html_buf, q->valuestring, &set,
                              (size_t)max_results) == 0) {
            online = 1;
        }
        free(html_buf);
    }

    /* 降级：生成搜索引擎链接（offline fallback，双引擎） */
    if (!online) {
        char url[WS_URL_LEN];
        char snip[WS_SNIPPET_LEN];
        snprintf(url, sizeof(url), "https://duckduckgo.com/?q=%s", encoded);
        snprintf(snip, sizeof(snip), "DuckDuckGo search results for: %.*s",
                 (int)(WS_SNIPPET_LEN - 40), q->valuestring);
        ws_add_result(&set, q->valuestring, url, snip, "duckduckgo", 1.0);
        snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", encoded);
        snprintf(snip, sizeof(snip), "Google search results for: %.*s",
                 (int)(WS_SNIPPET_LEN - 32), q->valuestring);
        ws_add_result(&set, q->valuestring, url, snip, "google", 0.9);
    }

    /* 去重 → 排序 → 截断 */
    ws_deduplicate(&set);
    ws_rank(&set, q->valuestring);
    if (set.count > (size_t)max_results)
        set.count = (size_t)max_results;

    /* 输出 JSON */
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddStringToObject(out, "query", q->valuestring);

    cJSON *results = cJSON_AddArrayToObject(out, "results");
    for (size_t i = 0; i < set.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "title", set.items[i].title);
        cJSON_AddStringToObject(item, "url", set.items[i].url);
        cJSON_AddStringToObject(item, "snippet", set.items[i].snippet);
        cJSON_AddStringToObject(item, "source", set.items[i].source);
        cJSON_AddItemToObject(item, "relevance",
                              ws_raw_num(set.items[i].relevance, 3));
        cJSON_AddItemToArray(results, item);
    }

    cJSON_AddNumberToObject(out, "total_found", (double)set.count);
    char summary[256];
    snprintf(summary, sizeof(summary), "Found %zu results for '%s' (%s)",
             set.count, q->valuestring, online ? "online" : "offline-links");
    cJSON_AddStringToObject(out, "summary", summary);
    cJSON_AddStringToObject(out, "mode", online ? "online" : "offline");
    cJSON_AddStringToObject(out, "engine", engine);
    (void)time_range;

    char *serialized = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!serialized)
        return -1;

    *json_output = serialized;
    return 0;
}
