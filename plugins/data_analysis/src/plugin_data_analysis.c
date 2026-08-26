// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file plugin_data_analysis.c
 * @brief data_analysis 技能插件（C 实现，真实逻辑）
 *
 * 对结构化/半结构化数据执行统计分析，移植自
 * ecosystem/skills/src/data_analysis.py：
 *   1. 数据解析：JSON 数组（对象列表）/ 单个 JSON 对象 / CSV 文本，
 *      format 参数支持 json/csv/auto（auto 自动检测）
 *   2. 数值字段检测：某字段超过 50% 记录值为数值 → 数值字段，
 *      target_fields 非空时过滤
 *   3. 描述性统计：count/mean/median/std_dev/min/max/p25/p75/p95/p99/
 *      iqr/skewness（百分位线性插值，总体标准差，偏度三阶中心矩）
 *   4. 异常检测：IQR 法与 Z-score 法（|z|>3），各返回 count + 前 10 个值
 *   5. 趋势检测：简单线性回归 slope/intercept/R²，方向 flat/upward/
 *      downward，change_rate_percent = slope/mean*100
 *   6. 洞察生成：样本量评估、异常值提示、显著趋势、偏态分布提示
 *   7. 输出 JSON：summary / record_count / fields_analyzed /
 *      descriptive_stats / outliers / trends / insights
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DA_MAX_FIELDS    64      /**< 最大字段数 */
#define DA_MAX_FIELDLEN  128     /**< 字段名最大长度 */
#define DA_MAX_RECORDS   65536   /**< 最大记录数 */
#define DA_MAX_OUTLIERS  10      /**< 异常值输出数量上限 */

/* ==================== 内部数据结构 ==================== */

/** 单列数据：字段名 + 按记录顺序收集的数值 */
typedef struct {
    char name[DA_MAX_FIELDLEN];
    double *values;          /* 数值数组（保持记录顺序） */
    size_t value_count;      /* 数值个数 */
    size_t value_cap;        /* 容量 */
    size_t present_count;    /* 该字段在记录中出现的次数 */
} da_column_t;

typedef struct {
    da_column_t cols[DA_MAX_FIELDS];
    size_t ncols;
    size_t nrows;            /* 有效记录数 */
} da_data_t;

/** 描述性统计计算结果（供洞察生成复用） */
typedef struct {
    double mean;
    double std_dev;
    double skew;             /* 仅 skew_valid=1 时有效 */
    int skew_valid;
} da_desc_result_t;

/** 趋势检测计算结果（供洞察生成复用） */
typedef struct {
    int active;
    char direction[16];
    double change_rate;
    double r_squared;
} da_trend_result_t;

/* 插件 user_data：生命周期状态 */
typedef struct {
    char config_path[256];
    char loaded_at[32];
    unsigned long execute_count;
    int running;
} da_user_data_t;

/* ==================== 元数据 ==================== */

static const plugin_metadata_t g_metadata = {
    .name = "data_analysis",
    .version = "1.0.0",
    .author = "SPHARX",
    .description =
        "Statistical analysis for structured data: descriptive statistics, "
        "IQR/Z-score outlier detection and linear trend regression",
    .type = PLUGIN_TYPE_TOOL_PROVIDER,
    .api_version = 1,
    .min_airy_version = 1,
};

/* ==================== 辅助函数 ==================== */

static char *da_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static void da_trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
}

static int da_col_add_value(da_column_t *col, double v)
{
    if (col->value_count >= col->value_cap) {
        size_t nc = col->value_cap ? col->value_cap * 2 : 16;
        double *nv = (double *)realloc(col->values, nc * sizeof(double));
        if (!nv)
            return -1;
        col->values = nv;
        col->value_cap = nc;
    }
    col->values[col->value_count++] = v;
    return 0;
}

static int da_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

/** 复制并排序（描述统计/异常检测需要排序，趋势检测保持原顺序） */
static double *da_sorted(const double *v, size_t n)
{
    double *c = (double *)malloc(n * sizeof(double));
    if (!c)
        return NULL;
    memcpy(c, v, n * sizeof(double));
    qsort(c, n, sizeof(double), da_cmp_double);
    return c;
}

static void da_data_free(da_data_t *d)
{
    if (!d)
        return;
    for (size_t i = 0; i < d->ncols; i++)
        free(d->cols[i].values);
    free(d);
}

/** 生成带指定位数小数的 JSON 原始数字节点（保证输出位数精确） */
static cJSON *da_raw_num(double v, int digits)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return cJSON_CreateRaw(buf);
}

/** 生成数值节点：整数值输出无小数（对齐 Python 原值语义），否则 4 位小数 */
static cJSON *da_raw_value(double v)
{
    char buf[64];
    if (fabs(v) < 1e15 && v == (double)(long long)v)
        snprintf(buf, sizeof(buf), "%.0f", v);
    else
        snprintf(buf, sizeof(buf), "%.4f", v);
    return cJSON_CreateRaw(buf);
}

/* ==================== 数据解析 ==================== */

static da_data_t *da_parse_json(const char *raw)
{
    cJSON *root = cJSON_Parse(raw);
    if (!root)
        return NULL;

    da_data_t *data = NULL;
    cJSON *recs[DA_MAX_RECORDS];
    size_t nrec = 0;

    if (cJSON_IsArray(root)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, root) {
            if (cJSON_IsObject(it) && nrec < DA_MAX_RECORDS)
                recs[nrec++] = it;
        }
    } else if (cJSON_IsObject(root)) {
        if (nrec < DA_MAX_RECORDS)
            recs[nrec++] = root;
    } else {
        cJSON_Delete(root);
        return NULL;
    }
    if (nrec == 0) {
        cJSON_Delete(root);
        return NULL;
    }

    data = (da_data_t *)calloc(1, sizeof(da_data_t));
    if (!data) {
        cJSON_Delete(root);
        return NULL;
    }

    /* 字段名：取自第一条记录的 item（对齐 Python records[0].keys()） */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, recs[0]) {
        if (data->ncols >= DA_MAX_FIELDS)
            break;
        if (item->string) {
            snprintf(data->cols[data->ncols].name, DA_MAX_FIELDLEN, "%s",
                     item->string);
            data->ncols++;
        }
    }
    if (data->ncols == 0) {
        da_data_free(data);
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t r = 0; r < nrec; r++) {
        for (size_t c = 0; c < data->ncols; c++) {
            cJSON *val =
                cJSON_GetObjectItemCaseSensitive(recs[r],
                                                 data->cols[c].name);
            if (!val)
                continue;
            data->cols[c].present_count++;
            if (cJSON_IsNumber(val))
                da_col_add_value(&data->cols[c], val->valuedouble);
        }
    }
    data->nrows = nrec;
    cJSON_Delete(root);
    return data;
}

static da_data_t *da_parse_csv(const char *raw)
{
    char *buf = da_strdup(raw);
    if (!buf)
        return NULL;
    da_data_t *data = (da_data_t *)calloc(1, sizeof(da_data_t));
    if (!data) {
        free(buf);
        return NULL;
    }

    int first = 1;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        da_trim(line);
        if (!*line)
            continue; /* 跳过空行 */

        if (first) {
            /* 表头行 */
            char *hsv = NULL;
            for (char *h = strtok_r(line, ",", &hsv); h;
                 h = strtok_r(NULL, ",", &hsv)) {
                da_trim(h);
                if (data->ncols >= DA_MAX_FIELDS)
                    break;
                snprintf(data->cols[data->ncols].name, DA_MAX_FIELDLEN, "%s",
                         h);
                data->ncols++;
            }
            first = 0;
            if (data->ncols == 0)
                break;
        } else {
            /* 数据行：按逗号拆分 */
            char *vals[DA_MAX_FIELDS];
            size_t nv = 0;
            char *vsv = NULL;
            for (char *v = strtok_r(line, ",", &vsv); v;
                 v = strtok_r(NULL, ",", &vsv)) {
                if (nv >= DA_MAX_FIELDS)
                    break;
                vals[nv++] = v;
            }
            if (nv != data->ncols)
                continue; /* 列数不匹配跳过 */

            data->nrows++;
            for (size_t i = 0; i < nv; i++) {
                da_trim(vals[i]);
                da_column_t *col = &data->cols[i];
                col->present_count++;
                int numeric = 0;
                double d = 0.0;
                if (strchr(vals[i], '.')) {
                    /* 含小数点 → double */
                    char *end = NULL;
                    d = strtod(vals[i], &end);
                    if (end != vals[i] && *end == '\0')
                        numeric = 1;
                } else {
                    /* 否则尝试 long */
                    char *end = NULL;
                    long lv = strtol(vals[i], &end, 10);
                    if (end != vals[i] && *end == '\0') {
                        d = (double)lv;
                        numeric = 1;
                    }
                }
                if (numeric)
                    da_col_add_value(col, d);
            }
        }
    }

    if (data->ncols == 0 || data->nrows == 0) {
        da_data_free(data);
        free(buf);
        return NULL;
    }
    free(buf);
    return data;
}

/** 解析入口：format 支持 json/csv/auto */
static da_data_t *da_parse(const char *raw, const char *fmt)
{
    const char *start = raw;
    size_t len = strlen(raw);
    while (len > 0 && isspace((unsigned char)raw[len - 1]))
        len--;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start >= raw + len)
        return NULL; /* 空白输入 */

    if (strcmp(fmt, "auto") == 0) {
        if (*start == '[' || *start == '{')
            return da_parse_json(start);
        else if (strchr(start, ',') && strchr(start, '\n'))
            return da_parse_csv(start);
        else
            return da_parse_json(start);
    } else if (strcmp(fmt, "json") == 0) {
        return da_parse_json(start);
    } else if (strcmp(fmt, "csv") == 0) {
        return da_parse_csv(start);
    }
    return NULL;
}

/* ==================== 数值字段检测 ==================== */

static size_t da_detect_numeric_fields(const da_data_t *data,
                                       const char *targets[], size_t ntargets,
                                       size_t out_idx[])
{
    size_t n = 0;
    for (size_t c = 0; c < data->ncols; c++) {
        const da_column_t *col = &data->cols[c];
        if (col->present_count == 0)
            continue;
        /* 超过 50% 记录值为数值 → 数值字段 */
        if (!(col->value_count > col->present_count * 0.5))
            continue;
        if (ntargets > 0) {
            int hit = 0;
            for (size_t t = 0; t < ntargets; t++) {
                if (strcmp(col->name, targets[t]) == 0) {
                    hit = 1;
                    break;
                }
            }
            if (!hit)
                continue;
        }
        out_idx[n++] = c;
    }
    return n;
}

/* ==================== 描述性统计 ==================== */

/** 线性插值百分位：k=(n-1)*pct/100，结果 = sorted[f] + (k-f)*(sorted[c]-sorted[f]) */
static double da_percentile(const double *sorted, size_t n, double pct)
{
    if (n == 0)
        return 0.0;
    double k = (double)(n - 1) * pct / 100.0;
    size_t f = (size_t)k;
    size_t c = f + 1;
    if (c >= n)
        return sorted[n - 1];
    return sorted[f] + (k - (double)f) * (sorted[c] - sorted[f]);
}

/** 偏度 = Σ(v-mean)³/n / std³（n<3 或 std==0 时为 0） */
static double da_skewness(const double *v, size_t n, double mean, double std)
{
    if (n < 3 || std == 0.0)
        return 0.0;
    double m3 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = v[i] - mean;
        m3 += d * d * d;
    }
    m3 /= (double)n;
    return m3 / (std * std * std);
}

static void da_descriptive(const da_column_t *col, cJSON *out_field,
                           da_desc_result_t *res)
{
    size_t n = col->value_count;
    if (n == 0)
        return;
    double *s = da_sorted(col->values, n);
    if (!s)
        return;

    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += s[i];
    double mean = sum / (double)n;

    double var = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = s[i] - mean;
        var += d * d;
    }
    var /= (double)n;
    double std = sqrt(var);

    double p25 = da_percentile(s, n, 25);
    double p75 = da_percentile(s, n, 75);
    double median = da_percentile(s, n, 50);
    double p95 = da_percentile(s, n, 95);
    double p99 = da_percentile(s, n, 99);

    double skew = 0.0;
    int skew_valid = 0;
    if (n >= 3 && std > 0.0) {
        skew = da_skewness(s, n, mean, std);
        skew_valid = 1;
    }

    cJSON_AddNumberToObject(out_field, "count", (double)n);
    cJSON_AddItemToObject(out_field, "mean", da_raw_num(mean, 4));
    cJSON_AddItemToObject(out_field, "median", da_raw_num(median, 4));
    cJSON_AddItemToObject(out_field, "std_dev", da_raw_num(std, 4));
    cJSON_AddItemToObject(out_field, "min", da_raw_value(s[0]));
    cJSON_AddItemToObject(out_field, "max", da_raw_value(s[n - 1]));
    cJSON_AddItemToObject(out_field, "p25", da_raw_num(p25, 4));
    cJSON_AddItemToObject(out_field, "p75", da_raw_num(p75, 4));
    cJSON_AddItemToObject(out_field, "p95", da_raw_num(p95, 4));
    cJSON_AddItemToObject(out_field, "p99", da_raw_num(p99, 4));
    cJSON_AddItemToObject(out_field, "iqr", da_raw_num(p75 - p25, 4));
    cJSON_AddItemToObject(out_field, "skewness",
                          skew_valid ? da_raw_num(skew, 4) : cJSON_CreateRaw("0"));

    if (res) {
        res->mean = mean;
        res->std_dev = std;
        res->skew = skew;
        res->skew_valid = skew_valid;
    }
    free(s);
}

/* ==================== 异常检测 ==================== */

/** 返回 IQR 法检出数量（供洞察生成汇总） */
static size_t da_outliers(const da_column_t *col, cJSON *out_field)
{
    size_t n = col->value_count;
    if (n < 4)
        return 0;
    double *s = da_sorted(col->values, n);
    if (!s)
        return 0;

    double q1 = da_percentile(s, n, 25);
    double q3 = da_percentile(s, n, 75);
    double iqr = q3 - q1;
    double lower = q1 - 1.5 * iqr;
    double upper = q3 + 1.5 * iqr;

    /* IQR 方法 */
    cJSON *iqr_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(iqr_obj, "lower_bound", da_raw_num(lower, 4));
    cJSON_AddItemToObject(iqr_obj, "upper_bound", da_raw_num(upper, 4));
    cJSON *ivals = cJSON_CreateArray();
    size_t icnt = 0;
    for (size_t i = 0; i < n && icnt < DA_MAX_OUTLIERS; i++) {
        if (s[i] < lower || s[i] > upper) {
            cJSON_AddItemToArray(ivals, da_raw_value(s[i]));
            icnt++;
        }
    }
    cJSON_AddNumberToObject(iqr_obj, "count", (double)icnt);
    cJSON_AddItemToObject(iqr_obj, "values", ivals);

    /* Z-score 方法 */
    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += s[i];
    double mean = sum / (double)n;
    double var = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = s[i] - mean;
        var += d * d;
    }
    var /= (double)n;
    double std = sqrt(var);

    cJSON *z_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(z_obj, "threshold", 3.0);
    cJSON *zvals = cJSON_CreateArray();
    size_t zcnt = 0;
    if (std > 0.0) {
        for (size_t i = 0; i < n && zcnt < DA_MAX_OUTLIERS; i++) {
            if (fabs((s[i] - mean) / std) > 3.0) {
                cJSON_AddItemToArray(zvals, da_raw_value(s[i]));
                zcnt++;
            }
        }
    }
    cJSON_AddNumberToObject(z_obj, "count", (double)zcnt);
    cJSON_AddItemToObject(z_obj, "values", zvals);

    cJSON_AddItemToObject(out_field, "iqr_method", iqr_obj);
    cJSON_AddItemToObject(out_field, "zscore_method", z_obj);
    free(s);
    return icnt;
}

/* ==================== 趋势检测 ==================== */

/** 简单线性回归：slope/intercept/r_squared（按记录原始顺序） */
static void da_trend(const da_column_t *col, cJSON *out_field,
                     da_trend_result_t *res)
{
    size_t n = col->value_count;
    if (n < 3)
        return; /* n≥3 才做 */
    const double *v = col->values;

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double x = (double)i;
        sum_x += x;
        sum_y += v[i];
        sum_xy += x * v[i];
        sum_x2 += x * x;
    }

    double denom = (double)n * sum_x2 - sum_x * sum_x;
    double slope, intercept, r2;
    if (denom == 0.0) {
        slope = 0.0;
        intercept = sum_y / (double)n;
        r2 = 0.0;
    } else {
        slope = ((double)n * sum_xy - sum_x * sum_y) / denom;
        intercept = (sum_y - slope * sum_x) / (double)n;
        double mean_y = sum_y / (double)n;
        double ss_tot = 0.0, ss_res = 0.0;
        for (size_t i = 0; i < n; i++) {
            double y = v[i];
            double x = (double)i;
            double dy = y - mean_y;
            ss_tot += dy * dy;
            double e = y - (slope * x + intercept);
            ss_res += e * e;
        }
        r2 = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 0.0;
    }

    const char *direction;
    if (fabs(slope) < 1e-10)
        direction = "flat";
    else if (slope > 0.0)
        direction = "upward";
    else
        direction = "downward";

    double mean_val = sum_y / (double)n;
    double change = mean_val != 0.0 ? slope / mean_val * 100.0 : 0.0;

    cJSON_AddStringToObject(out_field, "direction", direction);
    cJSON_AddItemToObject(out_field, "slope", da_raw_num(slope, 6));
    cJSON_AddItemToObject(out_field, "r_squared", da_raw_num(r2, 4));
    cJSON_AddItemToObject(out_field, "change_rate_percent",
                          da_raw_num(change, 2));
    cJSON_AddItemToObject(out_field, "start_value", da_raw_value(v[0]));
    cJSON_AddItemToObject(out_field, "end_value", da_raw_value(v[n - 1]));

    if (res) {
        res->active = 1;
        snprintf(res->direction, sizeof(res->direction), "%s", direction);
        res->change_rate = change;
        res->r_squared = r2;
    }
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

    da_user_data_t *ud = (da_user_data_t *)calloc(1, sizeof(da_user_data_t));
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
    da_user_data_t *ud = (da_user_data_t *)user_data;
    if (!ud)
        return -1;

    /* 自检：对内置样例执行一次真实分析，验证核心逻辑可用 */
    static const char sample[] =
        "field,value\nA,1\nB,2\nC,3\nD,10\nE,5\n";
    da_data_t *data = da_parse(sample, "csv");
    if (!data)
        return -1;
    size_t idx[DA_MAX_FIELDS];
    size_t nnum = da_detect_numeric_fields(data, NULL, 0, idx);
    int ok = (nnum == 1);
    da_data_free(data);
    if (!ok)
        return -1;

    ud->running = 1;
    return 0;
}

int plugin_stop(void *user_data)
{
    da_user_data_t *ud = (da_user_data_t *)user_data;
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
 * 输入 JSON：{"data": "...", "format": "auto", "analysis_type": "all",
 *             "target_fields": []}
 * 输出 JSON：{"summary": "...", "record_count": N,
 *             "fields_analyzed": [...], "descriptive_stats": {...},
 *             "outliers": {...}, "trends": {...}, "insights": [...]}
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

    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data_item) || data_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *fmt_item = cJSON_GetObjectItem(root, "format");
    const char *fmt = cJSON_IsString(fmt_item) ? fmt_item->valuestring : "auto";
    cJSON *atype_item = cJSON_GetObjectItem(root, "analysis_type");
    const char *atype =
        cJSON_IsString(atype_item) ? atype_item->valuestring : "all";

    /* target_fields：过滤数值字段 */
    const char *targets[DA_MAX_FIELDS];
    size_t ntargets = 0;
    cJSON *tf_item = cJSON_GetObjectItem(root, "target_fields");
    if (cJSON_IsArray(tf_item)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, tf_item) {
            if (cJSON_IsString(it) && ntargets < DA_MAX_FIELDS)
                targets[ntargets++] = it->valuestring;
        }
    }

    da_data_t *data = da_parse(data_item->valuestring, fmt);
    if (!data) {
        /* 数据存在但解析失败：对齐 Python 返回 record_count=0 的结果 */
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "summary", "No valid data found for analysis");
        cJSON_AddNumberToObject(out, "record_count", 0);
        cJSON_AddArrayToObject(out, "fields_analyzed");
        cJSON_AddObjectToObject(out, "descriptive_stats");
        cJSON_AddObjectToObject(out, "outliers");
        cJSON_AddObjectToObject(out, "trends");
        cJSON *ins = cJSON_AddArrayToObject(out, "insights");
        cJSON_AddItemToArray(ins,
                             cJSON_CreateString("Input data could not be parsed"));
        char *serialized = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        cJSON_Delete(root);
        if (!serialized)
            return -1;
        *json_output = serialized;
        return 0;
    }

    /* 检测数值字段 */
    size_t num_idx[DA_MAX_FIELDS];
    size_t nnum = da_detect_numeric_fields(data, targets, ntargets, num_idx);

    int do_desc = strcmp(atype, "descriptive") == 0 || strcmp(atype, "all") == 0;
    int do_outl = strcmp(atype, "outliers") == 0 || strcmp(atype, "all") == 0;
    int do_trend = strcmp(atype, "trend") == 0 || strcmp(atype, "all") == 0;

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        da_data_free(data);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *fields_arr = cJSON_AddArrayToObject(out, "fields_analyzed");
    cJSON *desc_obj = cJSON_AddObjectToObject(out, "descriptive_stats");
    cJSON *outl_obj = cJSON_AddObjectToObject(out, "outliers");
    cJSON *trend_obj = cJSON_AddObjectToObject(out, "trends");

    /* 洞察收集 */
    double skew_of[DA_MAX_FIELDS];
    int skew_ok[DA_MAX_FIELDS];
    size_t outlier_cnt[DA_MAX_FIELDS];
    da_trend_result_t trend_info[DA_MAX_FIELDS];

    for (size_t i = 0; i < nnum; i++) {
        da_column_t *col = &data->cols[num_idx[i]];
        skew_ok[i] = 0;
        outlier_cnt[i] = 0;
        memset(&trend_info[i], 0, sizeof(trend_info[i]));

        cJSON_AddItemToArray(fields_arr,
                             cJSON_CreateString(col->name));

        if (do_desc) {
            da_desc_result_t dr;
            memset(&dr, 0, sizeof(dr));
            cJSON *f = cJSON_CreateObject();
            da_descriptive(col, f, &dr);
            cJSON_AddItemToObject(desc_obj, col->name, f);
            skew_of[i] = dr.skew;
            skew_ok[i] = dr.skew_valid;
        }
        if (do_outl) {
            cJSON *f = cJSON_CreateObject();
            outlier_cnt[i] = da_outliers(col, f);
            cJSON_AddItemToObject(outl_obj, col->name, f);
        }
        if (do_trend) {
            cJSON *f = cJSON_CreateObject();
            da_trend(col, f, &trend_info[i]);
            cJSON_AddItemToObject(trend_obj, col->name, f);
        }
    }

    /* 洞察生成 */
    cJSON *insights = cJSON_AddArrayToObject(out, "insights");
    char buf[512];

    if (data->nrows < 30) {
        snprintf(buf, sizeof(buf),
                 "[Low confidence] Sample size (%zu) is small; "
                 "statistical conclusions may be unreliable",
                 data->nrows);
        cJSON_AddItemToArray(insights, cJSON_CreateString(buf));
    }

    size_t total_outliers = 0;
    for (size_t i = 0; i < nnum; i++)
        total_outliers += outlier_cnt[i];
    if (total_outliers > 0) {
        char flist[512] = "";
        size_t fpos = 0;
        int first_f = 1;
        for (size_t i = 0; i < nnum; i++) {
            if (outlier_cnt[i] > 0) {
                const char *fn = data->cols[num_idx[i]].name;
                size_t need = strlen(fn) + (first_f ? 0 : 2);
                if (fpos + need + 1 < sizeof(flist)) {
                    if (!first_f) {
                        flist[fpos++] = ',';
                        flist[fpos++] = ' ';
                    }
                    size_t fl = strlen(fn);
                    memcpy(flist + fpos, fn, fl);
                    fpos += fl;
                    first_f = 0;
                }
            }
        }
        flist[fpos] = '\0';
        snprintf(buf, sizeof(buf),
                 "Found %zu outlier(s) in field(s): %s",
                 total_outliers, flist);
        cJSON_AddItemToArray(insights, cJSON_CreateString(buf));
    }

    for (size_t i = 0; i < nnum; i++) {
        const da_trend_result_t *t = &trend_info[i];
        if (t->active && strcmp(t->direction, "flat") != 0 &&
            t->r_squared > 0.5) {
            snprintf(buf, sizeof(buf),
                     "%s shows a %s trend (%+.1f%% per period, R²=%.2f)",
                     data->cols[num_idx[i]].name, t->direction,
                     t->change_rate, t->r_squared);
            cJSON_AddItemToArray(insights, cJSON_CreateString(buf));
        }
    }

    for (size_t i = 0; i < nnum; i++) {
        if (skew_ok[i] && fabs(skew_of[i]) > 1.0) {
            const char *dir = skew_of[i] > 0.0 ? "right" : "left";
            snprintf(buf, sizeof(buf),
                     "%s has a %s-skewed distribution (skewness=%.2f)",
                     data->cols[num_idx[i]].name, dir, skew_of[i]);
            cJSON_AddItemToArray(insights, cJSON_CreateString(buf));
        }
    }

    if (cJSON_GetArraySize(insights) == 0) {
        cJSON_AddItemToArray(
            insights,
            cJSON_CreateString(
                "Data appears well-distributed with no significant anomalies"));
    }

    /* 摘要 */
    cJSON *first_ins = cJSON_GetArrayItem(insights, 0);
    const char *first_text =
        cJSON_IsString(first_ins) ? first_ins->valuestring : "";
    snprintf(buf, sizeof(buf),
             "Analyzed %zu records across %zu numeric field(s). "
             "Key insight: %s",
             data->nrows, nnum, first_text);
    cJSON_AddStringToObject(out, "summary", buf);
    cJSON_AddNumberToObject(out, "record_count", (double)data->nrows);

    char *serialized = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    da_data_free(data);
    cJSON_Delete(root);
    if (!serialized)
        return -1;

    *json_output = serialized;
    return 0;
}
