// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file plugin_text_summarization.c
 * @brief text_summarization 技能插件（C 实现，真实逻辑）
 *
 * 提取式文本摘要：
 *   1. 词频（TF）统计与关键词提取（top keywords）
 *   2. 按标点切分句子，基于 TF + 位置奖励（首句/末句）评分
 *   3. 在摘要预算内贪心选取关键句，输出保持原文顺序
 *   4. 输出 JSON：summary / key_points / top_keywords / 统计指标
 *
 * 插件 ABI（遵循 agentrt/daemons/plugin_d 约定，见 plugin_service.h）：
 *   - plugin_get_metadata()  （必需）
 *   - plugin_init()          （必需）
 *   - plugin_destroy()       （必需）
 *   - plugin_start()         （可选：内置样例自检）
 *   - plugin_stop()          （可选）
 *   - plugin_execute()       （扩展入口：JSON 入参 → JSON 出参，plugin_d 的
 *                            plugin.execute RPC 通过 dlsym 调用）
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

#define TS_MAX_WORDS      4096   /**< 最大不同词数 */
#define TS_WORD_LEN       64     /**< 单词最大长度 */
#define TS_MAX_SENTENCES  2048   /**< 最大句子数 */
#define TS_MAX_KEYWORDS   8      /**< 输出关键词数量 */
#define TS_MAX_POINTS     16     /**< 输出关键句数量上限 */
#define TS_SUMMARY_CAP    8192   /**< 摘要缓冲区大小 */

/* ==================== 内部数据结构 ==================== */

typedef struct {
    char word[TS_WORD_LEN];
    unsigned long freq;
} ts_word_t;

typedef struct {
    ts_word_t entries[TS_MAX_WORDS];
    size_t count;
    unsigned long total_tokens;
} ts_word_table_t;

typedef struct {
    const char *start;           /* 句子起始位置（文本副本内） */
    size_t len;                  /* 句子长度（含结尾标点） */
    unsigned long score;
} ts_sentence_t;

typedef struct {
    unsigned long total_chars;
    unsigned long word_count;
    unsigned long sentence_count;
    unsigned long distinct_words;
    double compression_ratio;
    char summary[TS_SUMMARY_CAP];
    char key_points[TS_MAX_POINTS][256];
    size_t key_point_count;
    char top_keywords[TS_MAX_KEYWORDS][TS_WORD_LEN];
    size_t keyword_count;
} ts_result_t;

/* 插件 user_data：生命周期状态（plugin.init/start/stop/execute 之间共享） */
typedef struct {
    char config_path[256];
    char loaded_at[32];
    unsigned long execute_count;
    int running;
} ts_user_data_t;

/* ==================== 元数据 ==================== */

static const plugin_metadata_t g_metadata = {
    .name = "text_summarization",
    .version = "1.0.0",
    .author = "SPHARX",
    .description =
        "Extractive text summarization: TF-based keyword extraction, "
        "key sentence scoring and compression ratio statistics",
    .type = PLUGIN_TYPE_TOOL_PROVIDER,
    .api_version = 1,
    .min_airy_version = 1,
};

/* ==================== 辅助函数 ==================== */

/**
 * @brief 判断 UTF-8 三字节 CJK 标点（。！？，、）
 * @note 调用方需保证 p[0]、p[1]、p[2] 均可读（文本副本尾部已补零）
 */
static int ts_punct3(const unsigned char *p)
{
    static const unsigned char punc[][3] = {
        {0xE3, 0x80, 0x82}, /* 。 */
        {0xEF, 0xBC, 0x81}, /* ！ */
        {0xEF, 0xBC, 0x9F}, /* ？ */
        {0xEF, 0xBC, 0x8C}, /* ， */
        {0xE3, 0x80, 0x81}, /* 、 */
    };
    for (size_t i = 0; i < sizeof(punc) / sizeof(punc[0]); i++) {
        if (p[0] == punc[i][0] && p[1] == punc[i][1] && p[2] == punc[i][2])
            return 1;
    }
    return 0;
}

/**
 * @brief 将词加入词频表（线性查找，词表容量上限 TS_MAX_WORDS）
 */
static void ts_table_add(ts_word_table_t *table, const char *word)
{
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].word, word) == 0) {
            table->entries[i].freq++;
            return;
        }
    }
    if (table->count < TS_MAX_WORDS) {
        size_t wlen = strlen(word);
        if (wlen >= TS_WORD_LEN) wlen = TS_WORD_LEN - 1;
        memcpy(table->entries[table->count].word, word, wlen);
        table->entries[table->count].word[wlen] = '\0';
        table->entries[table->count].freq = 1;
        table->count++;
    }
}

/**
 * @brief 词频统计
 * @param text 文本（调用方保证尾部有 ≥3 个 NUL 填充字节）
 */
static void ts_count_words(const char *text, ts_word_table_t *table)
{
    const char *p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            char buf[TS_WORD_LEN];
            size_t i = 0;
            while (*p && i + 1 < TS_WORD_LEN && isalnum((unsigned char)*p)) {
                buf[i++] = (char)tolower((unsigned char)*p);
                p++;
            }
            buf[i] = '\0';
            table->total_tokens++;
            /* 过滤长度为 1 的 token（虚词/噪声） */
            if (i >= 2) {
                ts_table_add(table, buf);
            }
        } else if (c >= 0x80) {
            if (ts_punct3((const unsigned char *)p)) {
                p += 3;
            } else {
                /* 非标点多字节字符按单字节 token 计（UTF-8 字节级近似） */
                table->total_tokens++;
                p++;
            }
        } else {
            p++;
        }
    }
}

/**
 * @brief 句子评分：词频加权（长度 ≥2 的 token 才参与），含位置奖励
 */
static unsigned long ts_sentence_score(const char *start, size_t len,
                                       const ts_word_table_t *table)
{
    unsigned long score = 0;
    const char *p = start;
    const char *end = start + len;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            char buf[TS_WORD_LEN];
            size_t i = 0;
            while (p < end && i + 1 < TS_WORD_LEN &&
                   isalnum((unsigned char)*p)) {
                buf[i++] = (char)tolower((unsigned char)*p);
                p++;
            }
            buf[i] = '\0';
            if (i >= 2) {
                for (size_t j = 0; j < table->count; j++) {
                    if (strcmp(table->entries[j].word, buf) == 0) {
                        score += table->entries[j].freq;
                        break;
                    }
                }
            }
        } else if (c >= 0x80 && ts_punct3((const unsigned char *)p)) {
            p += 3;
        } else {
            p++;
        }
    }
    return score;
}

/**
 * @brief 提取词频最高的前 TS_MAX_KEYWORDS 个关键词
 */
static void ts_top_keywords(const ts_word_table_t *table, ts_result_t *out)
{
    size_t order[TS_MAX_WORDS];
    for (size_t i = 0; i < table->count; i++)
        order[i] = i;

    /* 插入排序：按词频降序（稳定） */
    for (size_t i = 1; i < table->count; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 &&
               table->entries[order[j - 1]].freq < table->entries[key].freq) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    size_t n = table->count < TS_MAX_KEYWORDS ? table->count : TS_MAX_KEYWORDS;
    for (size_t i = 0; i < n; i++) {
        strncpy(out->top_keywords[out->keyword_count],
                table->entries[order[i]].word, TS_WORD_LEN - 1);
        out->top_keywords[out->keyword_count][TS_WORD_LEN - 1] = '\0';
        out->keyword_count++;
    }
}

/* ==================== 核心摘要逻辑 ==================== */

static int ts_run(const char *text, long max_length, ts_result_t *out)
{
    size_t text_len = strlen(text);
    if (text_len == 0) return -1;

    out->total_chars = (unsigned long)text_len;

    /* 文本副本（尾部补 3 字节 NUL，保证 ts_punct3 越界安全） */
    char *work = (char *)calloc(text_len + 4, 1);
    if (!work) return -1;
    memcpy(work, text, text_len);

    /* 1) 词频统计 */
    ts_word_table_t table;
    memset(&table, 0, sizeof(table));
    ts_count_words(work, &table);
    out->word_count = table.total_tokens;
    out->distinct_words = (unsigned long)table.count;

    /* 2) 句子切分（. ! ? \n 以及 CJK 。！？ 为句子边界） */
    ts_sentence_t sentences[TS_MAX_SENTENCES];
    size_t ns = 0;
    {
        char *p = work;
        char *start = work;
        while (*p) {
            int is_end = 0;
            size_t end_len = 1; /* 边界字符宽度（ASCII 1 字节；CJK 标点 3 字节） */
            if (*p == '.' || *p == '!' || *p == '?' || *p == '\n') {
                is_end = 1;
            } else if ((unsigned char)*p >= 0x80) {
                unsigned char c0 = (unsigned char)*p;
                if ((c0 == 0xE3 && (unsigned char)p[1] == 0x80 &&
                     (unsigned char)p[2] == 0x82) || /* 。 */
                    (c0 == 0xEF && (unsigned char)p[1] == 0xBC &&
                     ((unsigned char)p[2] == 0x81 ||
                      (unsigned char)p[2] == 0x9F))) { /* ！ ？ */
                    is_end = 1;
                    end_len = 3;
                }
            }
            if (is_end) {
                if (ns < TS_MAX_SENTENCES) {
                    sentences[ns].start = start;
                    sentences[ns].len = (size_t)(p - start) + end_len;
                    sentences[ns].score = 0;
                    ns++;
                }
                p += end_len;
                while (*p == ' ' || *p == '\t' || *p == '\n') p++;
                start = p;
            } else {
                p++;
            }
        }
        if (start < work + text_len && ns < TS_MAX_SENTENCES) {
            sentences[ns].start = start;
            sentences[ns].len = (size_t)(work + text_len - start);
            sentences[ns].score = 0;
            ns++;
        }
    }
    out->sentence_count = (unsigned long)ns;

    /* 3) 句子评分：TF 加权 + 位置奖励 */
    if (ns > 0) {
        unsigned long max_freq = 0;
        for (size_t i = 0; i < table.count; i++) {
            if (table.entries[i].freq > max_freq)
                max_freq = table.entries[i].freq;
        }
        for (size_t i = 0; i < ns; i++) {
            unsigned long score =
                ts_sentence_score(sentences[i].start, sentences[i].len, &table);
            if (i == 0)
                score += max_freq;             /* 首句奖励 */
            else if (i == ns - 1)
                score += max_freq / 2;         /* 末句奖励 */
            sentences[i].score = score;
        }
    }

    /* 4) 选择关键句：按分数降序贪心选取（预算内），输出保持原文顺序 */
    size_t order[TS_MAX_SENTENCES];
    size_t chosen[TS_MAX_SENTENCES];
    size_t nchosen = 0;
    if (ns > 0) {
        for (size_t i = 0; i < ns; i++)
            order[i] = i;
        for (size_t i = 1; i < ns; i++) {
            size_t key = order[i];
            size_t j = i;
            while (j > 0 && sentences[order[j - 1]].score < sentences[key].score) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = key;
        }
        long budget = max_length;
        for (size_t i = 0; i < ns && budget > 0; i++) {
            size_t idx = order[i];
            if ((long)sentences[idx].len <= budget || nchosen == 0) {
                chosen[nchosen++] = idx;
                budget -= (long)sentences[idx].len;
            }
        }
        /* 按原文顺序重排 */
        for (size_t i = 1; i < nchosen; i++) {
            size_t key = chosen[i];
            size_t j = i;
            while (j > 0 && sentences[chosen[j - 1]].start > sentences[key].start) {
                chosen[j] = chosen[j - 1];
                j--;
            }
            chosen[j] = key;
        }
    }

    /* 5) 拼装摘要与关键句列表 */
    size_t sum_len = 0;
    char *dst = out->summary;
    for (size_t i = 0; i < nchosen; i++) {
        size_t idx = chosen[i];
        size_t take = sentences[idx].len;
        if (take >= TS_SUMMARY_CAP - sum_len - 1)
            take = TS_SUMMARY_CAP - sum_len - 1;
        if (take == 0) break;
        memcpy(dst + sum_len, sentences[idx].start, take);
        sum_len += take;
        dst[sum_len++] = ' ';

        /* key_points：修剪首尾空白后截取 */
        const char *s = sentences[idx].start;
        size_t kl = sentences[idx].len;
        while (kl > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) {
            s++;
            kl--;
        }
        while (kl > 0 && (s[kl - 1] == ' ' || s[kl - 1] == '\t' ||
                          s[kl - 1] == '\n'))
            kl--;
        if (kl > 0 && out->key_point_count < TS_MAX_POINTS) {
            size_t cplen = kl < 255 ? kl : 255;
            memcpy(out->key_points[out->key_point_count], s, cplen);
            out->key_points[out->key_point_count][cplen] = '\0';
            out->key_point_count++;
        }
    }
    if (sum_len > 0 && dst[sum_len - 1] == ' ')
        sum_len--;
    if (sum_len == 0) {
        /* 兜底：无法切分时直接截取原文前缀 */
        size_t take = text_len < (size_t)max_length ? text_len
                                                    : (size_t)max_length;
        take = take < TS_SUMMARY_CAP - 1 ? take : TS_SUMMARY_CAP - 1;
        memcpy(dst, text, take);
        sum_len = take;
    }
    dst[sum_len] = '\0';

    size_t summary_len = strlen(dst);
    if (out->total_chars > 0) {
        out->compression_ratio =
            (1.0 - (double)summary_len / (double)out->total_chars) * 100.0;
        if (out->compression_ratio < 0.0) out->compression_ratio = 0.0;
    }

    /* 6) 关键词 */
    ts_top_keywords(&table, out);

    free(work);
    return 0;
}

/* ==================== 插件 ABI 实现 ==================== */

const plugin_metadata_t *plugin_get_metadata(void)
{
    return &g_metadata;
}

int plugin_init(const char *config_path, void **user_data)
{
    if (!user_data) return -1;

    ts_user_data_t *ud = (ts_user_data_t *)calloc(1, sizeof(ts_user_data_t));
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
    ts_user_data_t *ud = (ts_user_data_t *)user_data;
    if (!ud) return -1;

    /* 自检：对内置样例执行一次真实摘要，验证核心逻辑可用 */
    static const char sample[] =
        "AgentRT is a runtime for building intelligent agents. "
        "It provides memory, scheduling and skill subsystems. "
        "Plugins extend AgentRT with new capabilities. "
        "The plugin daemon discovers and loads skill plugins. "
        "Text summarization is one of the built-in skills.";

    ts_result_t res;
    memset(&res, 0, sizeof(res));
    if (ts_run(sample, 200, &res) != 0 || res.key_point_count == 0)
        return -1;

    ud->running = 1;
    return 0;
}

int plugin_stop(void *user_data)
{
    ts_user_data_t *ud = (ts_user_data_t *)user_data;
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
 * 输入 JSON：{"text": "...", "max_length": 500, "mode": "extractive"}
 * 输出 JSON：{"ok": true, "summary": "...", "key_points": [...],
 *            "top_keywords": [...], "total_characters": N, ...}
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

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_item) || text_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *max_item = cJSON_GetObjectItem(root, "max_length");
    long max_length =
        cJSON_IsNumber(max_item) ? (long)max_item->valuedouble : 500L;
    if (max_length <= 0) max_length = 500;

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    const char *mode =
        cJSON_IsString(mode_item) ? mode_item->valuestring : "extractive";

    ts_result_t result;
    memset(&result, 0, sizeof(result));
    if (ts_run(text_item->valuestring, max_length, &result) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "summary", result.summary);
    cJSON_AddStringToObject(out, "mode", mode);
    cJSON_AddNumberToObject(out, "total_characters",
                            (double)result.total_chars);
    cJSON_AddNumberToObject(out, "word_count", (double)result.word_count);
    cJSON_AddNumberToObject(out, "sentence_count",
                            (double)result.sentence_count);
    cJSON_AddNumberToObject(out, "distinct_words",
                            (double)result.distinct_words);
    cJSON_AddNumberToObject(out, "compression_ratio",
                            result.compression_ratio);

    cJSON *points = cJSON_AddArrayToObject(out, "key_points");
    for (size_t i = 0; i < result.key_point_count; i++) {
        cJSON_AddItemToArray(points, cJSON_CreateString(result.key_points[i]));
    }
    cJSON *kw = cJSON_AddArrayToObject(out, "top_keywords");
    for (size_t i = 0; i < result.keyword_count; i++) {
        cJSON_AddItemToArray(kw, cJSON_CreateString(result.top_keywords[i]));
    }

    char *serialized = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!serialized) return -1;

    *json_output = serialized;
    return 0;
}
