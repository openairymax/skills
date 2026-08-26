// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file plugin_security_audit.c
 * @brief security_audit 技能插件（C 实现，真实逻辑）
 *
 * 对目标内容做安全审计（移植自 ecosystem/skills/src/security_audit.py）：
 *   1. 审计类型自动检测（config/dependencies/permissions/network/compliance）
 *   2. 正则式快速扫描（按类型过滤）：硬编码密钥、危险配置、权限、
 *      网络暴露、依赖版本等
 *   3. 逐行静态审计：HTTP/MD5/CORS/chmod/USER root/--privileged/TLS/telnet/ftp
 *   4. 已知漏洞依赖库映射（requests/urllib3/certifi/cryptography/jinja2/pyyaml
 *      + CVE），版本按数字分段比较
 *   5. 风险评分：critical×10+high×6+medium×3+low×1+info×0.5，×2 后 clamp [0,100]
 *   6. 去重、severity 排序、severity_threshold 过滤
 *   7. 输出 JSON：audit_type / framework / risk_score / risk_level /
 *      findings[] / summary / recommendations[]
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

#define SA_MAX_FINDINGS 512   /**< findings 数组容量 */
#define SA_TITLE_LEN     96
#define SA_DESC_LEN      256
#define SA_REMED_LEN     128

/* ==================== 内部数据结构 ==================== */

typedef struct {
    char id[16];                /* qs-N / cfg-N / dep-N / perm-N / net-N */
    char severity[12];          /* critical/high/medium/low/info */
    char category[16];          /* config/dependencies/permissions/network */
    char title[SA_TITLE_LEN];
    char description[SA_DESC_LEN];
    char remediation[SA_REMED_LEN];
    char cve_ref[24];           /* 依赖漏洞的 CVE 编号（可空） */
} sa_finding_t;

typedef struct {
    sa_finding_t items[SA_MAX_FINDINGS];
    size_t count;
} sa_list_t;

/* 插件 user_data：生命周期状态 */
typedef struct {
    char config_path[256];
    char loaded_at[32];
    unsigned long execute_count;
    int running;
} sa_user_data_t;

/* ==================== 元数据 ==================== */

static const plugin_metadata_t g_metadata = {
    .name = "security_audit",
    .version = "1.0.0",
    .author = "SPHARX",
    .description =
        "Security audit of configs, code and dependencies: pattern scanning, "
        "known-CVE mapping and risk scoring",
    .type = PLUGIN_TYPE_TOOL_PROVIDER,
    .api_version = 1,
    .min_airy_version = 1,
};

/* ==================== 辅助函数 ==================== */

static int sa_isspace_c(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

/* 大小写不敏感比较（UTF-8 安全：仅对 ASCII 归一化） */
static int sa_icmp(const char *a, const char *b)
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

/* 大小写不敏感前缀比较（n 字节） */
static int sa_incmp(const char *s, const char *prefix, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!s[i]) return -1;
        int ca = tolower((unsigned char)s[i]);
        int cb = tolower((unsigned char)prefix[i]);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* 大小写不敏感子串查找 */
static const char *sa_ifind(const char *text, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return text;
    for (const char *p = text; *p; p++) {
        if (sa_incmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

/* 追加一条 finding（cve 可为 NULL） */
static void sa_add(sa_list_t *list, const char *id, const char *sev,
                   const char *cat, const char *title, const char *desc,
                   const char *remed, const char *cve)
{
    if (list->count >= SA_MAX_FINDINGS) return;
    sa_finding_t *f = &list->items[list->count++];
    memset(f, 0, sizeof(*f));
    strncpy(f->id, id, sizeof(f->id) - 1);
    strncpy(f->severity, sev, sizeof(f->severity) - 1);
    strncpy(f->category, cat, sizeof(f->category) - 1);
    strncpy(f->title, title, sizeof(f->title) - 1);
    strncpy(f->description, desc, sizeof(f->description) - 1);
    strncpy(f->remediation, remed, sizeof(f->remediation) - 1);
    if (cve) strncpy(f->cve_ref, cve, sizeof(f->cve_ref) - 1);
}

/* ==================== 审计类型自动检测 ==================== */

static const char *sa_detect_type(const char *target)
{
    static const char *cfg_kw[] = {"dockerfile", "docker-compose", "nginx",
                                   "apache", "toml", "yaml", "json", "ini",
                                   "conf"};
    static const char *dep_kw[] = {"requirements.txt", "package.json",
                                   "cargo.toml", "go.mod", "pom.xml",
                                   "gemfile", "dependency"};
    static const char *perm_kw[] = {"chmod", "permission", "rwx", "owner",
                                    "acl", "sudo", "setuid"};
    static const char *net_kw[] = {"port", "firewall", "tls", "ssl",
                                   "certificate", "dns", "subnet",
                                   "open port"};
    static const char *comp_kw[] = {"owasp", "nist", "iso", "gdpr", "hipaa",
                                    "pci", "soc2", "compliance"};

    for (size_t i = 0; i < sizeof(cfg_kw) / sizeof(cfg_kw[0]); i++)
        if (sa_ifind(target, cfg_kw[i])) return "config";
    for (size_t i = 0; i < sizeof(dep_kw) / sizeof(dep_kw[0]); i++)
        if (sa_ifind(target, dep_kw[i])) return "dependencies";
    for (size_t i = 0; i < sizeof(perm_kw) / sizeof(perm_kw[0]); i++)
        if (sa_ifind(target, perm_kw[i])) return "permissions";
    for (size_t i = 0; i < sizeof(net_kw) / sizeof(net_kw[0]); i++)
        if (sa_ifind(target, net_kw[i])) return "network";
    for (size_t i = 0; i < sizeof(comp_kw) / sizeof(comp_kw[0]); i++)
        if (sa_ifind(target, comp_kw[i])) return "compliance";
    return "config";
}

/* ==================== 快速扫描（正则式，大小写不敏感，按类型过滤） ==================== */

/* key 空白 [=:] 空白 "..."（引号内 1..30 字符） */
static int sa_key_value(const char *text, const char *key, int quote_lo,
                        int quote_hi)
{
    const char *p = text;
    while ((p = sa_ifind(p, key)) != NULL) {
        const char *q = p + strlen(key);
        while (*q && sa_isspace_c(*q)) q++;
        if (*q == '=' || *q == ':') {
            q++;
            while (*q && sa_isspace_c(*q)) q++;
            if (*q == '"' || *q == '\'') {
                char quote = *q;
                const char *r = q + 1;
                size_t cnt = 0;
                while (*r && *r != quote && cnt < (size_t)quote_hi + 1) {
                    cnt++;
                    r++;
                }
                if (cnt >= (size_t)quote_lo && cnt <= (size_t)quote_hi &&
                    *r == quote)
                    return 1;
            }
        }
        p = q;
    }
    return 0;
}

/* key 空白 [=:] 空白 true */
static int sa_bool_true(const char *text, const char *key)
{
    const char *p = text;
    while ((p = sa_ifind(p, key)) != NULL) {
        const char *q = p + strlen(key);
        while (*q && sa_isspace_c(*q)) q++;
        if (*q == '=' || *q == ':') {
            q++;
            while (*q && sa_isspace_c(*q)) q++;
            if (sa_incmp(q, "true", 4) == 0 &&
                !isalnum((unsigned char)q[4]))
                return 1;
        }
        p = q;
    }
    return 0;
}

/* ALLOWED_HOSTS = ['*'] */
static int sa_allowed_hosts(const char *text)
{
    const char *p = text;
    while ((p = sa_ifind(p, "allowed_hosts")) != NULL) {
        const char *q = p + strlen("allowed_hosts");
        while (*q && sa_isspace_c(*q)) q++;
        if (*q != '=') { p = q; continue; }
        q++;
        while (*q && sa_isspace_c(*q)) q++;
        if (*q != '[') { p = q; continue; }
        q++;
        while (*q && sa_isspace_c(*q)) q++;
        if (*q != '"' && *q != '\'') { p = q; continue; }
        if (q[1] != '*') { p = q; continue; }
        if (q[2] != q[0]) { p = q; continue; }
        q += 3;
        while (*q && sa_isspace_c(*q)) q++;
        if (*q == ']') return 1;
        p = q;
    }
    return 0;
}

/* pkg 空白 分隔符(=,<,> 可选 =) 空白 可选引号 版本号 2.N（返回版本数字位置） */
static const char *sa_pkg_version(const char *text, const char *pkg,
                                  char *ver, size_t ver_sz)
{
    const char *p = text;
    while ((p = sa_ifind(p, pkg)) != NULL) {
        const char *q = p + strlen(pkg);
        while (*q && sa_isspace_c(*q)) q++;
        if (*q != '=' && *q != '<' && *q != '>') { p = q; continue; }
        q++;
        if (*q == '=') q++;   /* <= >= == */
        while (*q && sa_isspace_c(*q)) q++;
        if (*q == '"' || *q == '\'') q++;
        /* 解析版本号：数字 + ('.' 数字)* */
        const char *vstart = q;
        while (*q && (isdigit((unsigned char)*q) || *q == '.')) q++;
        if (q > vstart && *vstart >= '0' && *vstart <= '9') {
            size_t len = (size_t)(q - vstart);
            if (len >= ver_sz) len = ver_sz - 1;
            memcpy(ver, vstart, len);
            ver[len] = '\0';
            return q;
        }
        p = q;
    }
    return NULL;
}

/* chmod 空白 777 */
static int sa_chmod_777(const char *text)
{
    const char *p = text;
    while ((p = sa_ifind(p, "chmod")) != NULL) {
        const char *q = p + 5;
        while (*q && sa_isspace_c(*q)) q++;
        if (sa_incmp(q, "777", 3) == 0 &&
            !isdigit((unsigned char)q[3]))
            return 1;
        p = q;
    }
    return 0;
}

/* chmod 空白 o+w */
static int sa_chmod_ow(const char *text)
{
    const char *p = text;
    while ((p = sa_ifind(p, "chmod")) != NULL) {
        const char *q = p + 5;
        while (*q && sa_isspace_c(*q)) q++;
        if (sa_incmp(q, "o+w", 3) == 0) return 1;
        p = q;
    }
    return 0;
}

/* USER 空白 root 或 RUN 空白 useradd */
static int sa_user_root(const char *text)
{
    const char *p = text;
    while ((p = sa_ifind(p, "user")) != NULL) {
        const char *q = p + 4;
        while (*q && sa_isspace_c(*q)) q++;
        if (sa_incmp(q, "root", 4) == 0 &&
            !isalnum((unsigned char)q[4]))
            return 1;
        p = q;
    }
    p = text;
    while ((p = sa_ifind(p, "run")) != NULL) {
        const char *q = p + 3;
        while (*q && sa_isspace_c(*q)) q++;
        if (sa_incmp(q, "useradd", 7) == 0 &&
            !isalnum((unsigned char)q[7]))
            return 1;
        p = q;
    }
    return 0;
}

/* SSLv2/SSLv3 / TLSv1.0 / TLSv1.1 / 裸 TLSv1（不含 TLSv1.2/1.3） */
static int sa_ssl_tls(const char *text)
{
    const char *p = text;
    while ((p = sa_ifind(p, "sslv")) != NULL) {
        char c = tolower((unsigned char)p[4]);
        if ((c == '2' || c == '3') &&
            !isalnum((unsigned char)p[5]) && p[5] != '.')
            return 1;
        p = p + 4;
    }
    p = text;
    while ((p = sa_ifind(p, "tlsv1")) != NULL) {
        const char *q = p + 5;
        if (*q == '.') {
            if (q[1] == '0' || q[1] == '1') return 1;  /* TLSv1.0 / TLSv1.1 */
        } else if (!isalnum((unsigned char)*q)) {
            return 1;                                   /* 裸 TLSv1 */
        }
        p = q;
    }
    return 0;
}

/* 快速扫描：仅在 audit_type 为该 category（或 auto 全量）时启用 */
static void sa_quick_scan(const char *target, const char *audit_type,
                          sa_list_t *list)
{
    static const char *T_CONFIG = "config";
    static const char *T_DEPS = "dependencies";
    static const char *T_PERM = "permissions";
    static const char *T_NET = "network";
    int is_auto = strcmp(audit_type, "auto") == 0;
    size_t qs = 0;
    char id[16];

#define SA_QS(cat, cond, sev, title, remed)                               \
    do {                                                                  \
        if ((is_auto || strcmp(audit_type, cat) == 0) && (cond)) {        \
            snprintf(id, sizeof(id), "qs-%zu", ++qs);                     \
            char d[SA_DESC_LEN];                                          \
            snprintf(d, sizeof(d), "Pattern detected: %s", title);        \
            sa_add(list, id, sev, cat, title, d, remed, NULL);            \
        }                                                                 \
    } while (0)

    SA_QS(T_CONFIG, sa_key_value(target, "password", 1, 30), "critical",
          "Hardcoded password in configuration",
          "Review and remediate the identified issue");
    SA_QS(T_CONFIG, sa_key_value(target, "api_key", 10, 128) ||
                    sa_key_value(target, "apikey", 10, 128) ||
                    sa_key_value(target, "api-key", 10, 128), "high",
          "Hardcoded API key detected",
          "Review and remediate the identified issue");
    SA_QS(T_CONFIG, sa_key_value(target, "secret", 1, 128), "high",
          "Hardcoded secret in configuration",
          "Review and remediate the identified issue");
    SA_QS(T_CONFIG, sa_bool_true(target, "debug"), "medium",
          "Debug mode enabled in production config",
          "Review and remediate the identified issue");
    SA_QS(T_CONFIG, sa_allowed_hosts(target), "high",
          "Wildcard ALLOWED_HOSTS — potential host header injection",
          "Review and remediate the identified issue");

    {
        char ver[32];
        if (is_auto || strcmp(audit_type, T_DEPS) == 0) {
            if (sa_pkg_version(target, "requests", ver, sizeof(ver)) &&
                ver[0] == '2' && ver[1] == '.') {
                /* 快速扫描：2.x 一律提示（静态审计再按版本精确判定） */
                snprintf(id, sizeof(id), "qs-%zu", ++qs);
                sa_add(list, id, "high", T_DEPS,
                       "Outdated requests library — potential CVE",
                       "Pattern detected: Outdated requests library — "
                       "potential CVE",
                       "Review and remediate the identified issue", NULL);
            }
            if (sa_pkg_version(target, "django", ver, sizeof(ver)) &&
                (ver[0] == '1' || ver[0] == '2') && ver[1] == '.') {
                snprintf(id, sizeof(id), "qs-%zu", ++qs);
                sa_add(list, id, "medium", T_DEPS,
                       "Older Django version — check for security patches",
                       "Pattern detected: Older Django version — check for "
                       "security patches",
                       "Review and remediate the identified issue", NULL);
            }
        }
    }

    SA_QS(T_PERM, sa_chmod_777(target), "high",
          "World-writable permissions (777) detected",
          "Review and remediate the identified issue");
    SA_QS(T_PERM, sa_chmod_ow(target), "medium",
          "World-writable permission added",
          "Review and remediate the identified issue");
    SA_QS(T_PERM, sa_user_root(target), "high",
          "Container running as root",
          "Review and remediate the identified issue");

    SA_QS(T_NET, sa_ifind(target, "0.0.0.0") != NULL, "medium",
          "Binding to all interfaces (0.0.0.0)",
          "Review and remediate the identified issue");
    SA_QS(T_NET, sa_ssl_tls(target), "high",
          "Deprecated SSL/TLS version detected",
          "Review and remediate the identified issue");
#undef SA_QS
}

/* ==================== 静态审计（逐行） ==================== */

/* 版本解析：分段数字（最多 8 段），返回段数 */
static size_t sa_parse_version(const char *v, unsigned *out)
{
    size_t n = 0;
    const char *p = v;
    while (*p && n < 8) {
        while (*p == '.' || *p == ' ') p++;
        unsigned val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10u + (unsigned)(*p - '0');
            if (val > 1000000u) val = 1000000u;
            p++;
            digits++;
        }
        if (digits == 0) break;
        out[n++] = val;
        if (*p != '.' && *p != ' ') break;
    }
    return n;
}

/* 版本比较 v1 <= v2（按数字分段） */
static int sa_version_le(const char *v1, const char *v2)
{
    unsigned a[8] = {0}, b[8] = {0};
    size_t na = sa_parse_version(v1, a);
    size_t nb = sa_parse_version(v2, b);
    size_t n = na > nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        unsigned va = i < na ? a[i] : 0;
        unsigned vb = i < nb ? b[i] : 0;
        if (va < vb) return 1;
        if (va > vb) return 0;
    }
    return 1; /* 相等 → <= */
}

/* 依赖审计：已知漏洞库映射 */
static void sa_audit_dependencies(const char *target, sa_list_t *list)
{
    static const struct {
        const char *pkg;
        const char *ver;    /* 受影响最高版本（<= 判定） */
        const char *cve;
        const char *sev;
        const char *fix;
    } vulns[] = {
        {"requests", "2.31.0", "CVE-2024-35195", "medium",
         "Upgrade requests to >= 2.32.0"},
        {"urllib3", "2.0.7", "CVE-2024-37891", "medium",
         "Upgrade urllib3 to >= 2.2.0"},
        {"certifi", "2023.7.22", "CVE-2024-39689", "low",
         "Upgrade certifi to >= 2024.7.4"},
        {"cryptography", "42.0.0", "CVE-2024-26130", "high",
         "Upgrade cryptography to >= 42.0.4"},
        {"jinja2", "3.1.2", "CVE-2024-34064", "medium",
         "Upgrade Jinja2 to >= 3.1.4"},
        {"pyyaml", "6.0", "CVE-2020-14343", "high",
         "Upgrade PyYAML to >= 6.0.1"},
    };
    size_t dep_no = 0;
    char id[16];

    /* 逐行扫描，每行每包最多一条 */
    const char *p = target;
    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        size_t take = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = '\0';

        for (size_t v = 0; v < sizeof(vulns) / sizeof(vulns[0]); v++) {
            char ver[32];
            if (!sa_pkg_version(line, vulns[v].pkg, ver, sizeof(ver)))
                continue;
            if (!sa_version_le(ver, vulns[v].ver))
                continue;
            char title[SA_TITLE_LEN], desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "dep-%zu", ++dep_no);
            snprintf(title, sizeof(title), "Vulnerable dependency: %s %s",
                     vulns[v].pkg, ver);
            snprintf(desc, sizeof(desc), "%s %s is affected by %s",
                     vulns[v].pkg, ver, vulns[v].cve);
            sa_add(list, id, vulns[v].sev, "dependencies", title, desc,
                   vulns[v].fix, vulns[v].cve);
        }

        if (!eol) break;
        p = eol + 1;
    }
}

/* 配置静态审计（逐行） */
static void sa_audit_config(const char *target, sa_list_t *list)
{
    size_t cfg_no = 0;
    char id[16];
    const char *p = target;
    int lineno = 1;

    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* 非 localhost 的 http:// URL */
        if (sa_ifind(p, "http://") &&
            !sa_ifind(p, "localhost") && !sa_ifind(p, "127.0.0.1")) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "cfg-%zu", ++cfg_no);
            snprintf(desc, sizeof(desc),
                     "Line %d: Non-localhost HTTP URL detected", lineno);
            sa_add(list, id, "medium", "config",
                   "HTTP used instead of HTTPS", desc,
                   "Use HTTPS for all production endpoints", NULL);
        }

        /* MD5 / SHA-1 过期哈希 */
        if (sa_ifind(p, "md5") || sa_ifind(p, "sha1")) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "cfg-%zu", ++cfg_no);
            snprintf(desc, sizeof(desc),
                     "Line %d: MD5 or SHA-1 detected", lineno);
            sa_add(list, id, "high", "config",
                   "Deprecated hash algorithm", desc,
                   "Use SHA-256 or stronger hashing algorithm", NULL);
        }

        /* 宽松 CORS：Access-Control-Allow-Origin: * */
        if (sa_ifind(p, "access-control-allow-origin")) {
            const char *q = sa_ifind(p, "access-control-allow-origin");
            const char *r = q + strlen("access-control-allow-origin");
            while (*r && sa_isspace_c(*r)) r++;
            if (*r == ':') {
                r++;
                while (*r && sa_isspace_c(*r)) r++;
                if (*r == '*') {
                    char desc[SA_DESC_LEN];
                    snprintf(id, sizeof(id), "cfg-%zu", ++cfg_no);
                    snprintf(desc, sizeof(desc),
                             "Line %d: Access-Control-Allow-Origin set to "
                             "wildcard", lineno);
                    sa_add(list, id, "medium", "config",
                           "Overly permissive CORS policy", desc,
                           "Restrict CORS to specific trusted origins", NULL);
                }
            }
        }

        if (!eol) break;
        p = eol + 1;
        lineno++;
    }
}

/* 权限静态审计（逐行） */
static void sa_audit_permissions(const char *target, sa_list_t *list)
{
    size_t perm_no = 0;
    char id[16];
    const char *p = target;
    int lineno = 1;

    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        size_t take = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = '\0';

        if (sa_chmod_777(line)) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "perm-%zu", ++perm_no);
            snprintf(desc, sizeof(desc), "Line %d: chmod 777 detected",
                     lineno);
            sa_add(list, id, "high", "permissions",
                   "World-writable file permissions (777)", desc,
                   "Use restrictive permissions (e.g., 644 for files, 755 "
                   "for dirs)", NULL);
        }
        if (sa_chmod_ow(line)) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "perm-%zu", ++perm_no);
            snprintf(desc, sizeof(desc), "Line %d: chmod o+w detected",
                     lineno);
            sa_add(list, id, "medium", "permissions",
                   "World-writable permission added", desc,
                   "Consider using group permissions instead", NULL);
        }
        if (sa_user_root(line)) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "perm-%zu", ++perm_no);
            snprintf(desc, sizeof(desc),
                     "Line %d: USER root in Dockerfile", lineno);
            sa_add(list, id, "high", "permissions",
                   "Container running as root", desc,
                   "Create and use a non-root user for the container", NULL);
        }
        if (sa_ifind(line, "--privileged")) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "perm-%zu", ++perm_no);
            snprintf(desc, sizeof(desc),
                     "Line %d: --privileged flag detected", lineno);
            sa_add(list, id, "critical", "permissions",
                   "Privileged container mode", desc,
                   "Remove --privileged; use --cap-add for specific "
                   "capabilities", NULL);
        }

        if (!eol) break;
        p = eol + 1;
        lineno++;
    }
}

/* 网络静态审计（逐行） */
static void sa_audit_network(const char *target, sa_list_t *list)
{
    size_t net_no = 0;
    char id[16];
    const char *p = target;
    int lineno = 1;

    while (1) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        size_t take = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = '\0';

        if (sa_ssl_tls(line)) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "net-%zu", ++net_no);
            snprintf(desc, sizeof(desc),
                     "Line %d: SSLv3 or TLS 1.0/1.1 detected", lineno);
            sa_add(list, id, "high", "network",
                   "Deprecated SSL/TLS protocol", desc,
                   "Use TLS 1.2 or TLS 1.3 minimum", NULL);
        }
        if (sa_ifind(line, "telnet")) {
            char desc[SA_DESC_LEN];
            snprintf(id, sizeof(id), "net-%zu", ++net_no);
            snprintf(desc, sizeof(desc), "Line %d: telnet detected", lineno);
            sa_add(list, id, "critical", "network",
                   "Telnet protocol detected",
                   "Telnet transmits data in plaintext",
                   "Replace telnet with SSH", NULL);
        }
        /* ftp（词边界且后不跟 s，排除 ftps） */
        {
            const char *f = line;
            int ftp_found = 0;
            while ((f = sa_ifind(f, "ftp")) != NULL) {
                int prev_ok = (f == line) ||
                              !isalnum((unsigned char)f[-1]) &&
                                  f[-1] != '_';
                int next_c = (unsigned char)f[3];
                int next_ok = !isalnum(next_c) && next_c != '_' &&
                              tolower(next_c) != 's';
                if (prev_ok && next_ok) { ftp_found = 1; break; }
                f += 3;
            }
            if (ftp_found) {
                char desc[SA_DESC_LEN];
                snprintf(id, sizeof(id), "net-%zu", ++net_no);
                snprintf(desc, sizeof(desc),
                         "Line %d: FTP transmits credentials in plaintext",
                         lineno);
                sa_add(list, id, "high", "network",
                       "Unencrypted FTP detected", desc,
                       "Use SFTP or FTPS instead", NULL);
            }
        }

        if (!eol) break;
        p = eol + 1;
        lineno++;
    }
}

/* 静态审计分发：auto 时全类型执行 */
static void sa_static_audit(const char *target, const char *audit_type,
                            sa_list_t *list)
{
    int is_auto = strcmp(audit_type, "auto") == 0;
    if (is_auto || strcmp(audit_type, "config") == 0)
        sa_audit_config(target, list);
    if (is_auto || strcmp(audit_type, "dependencies") == 0)
        sa_audit_dependencies(target, list);
    if (is_auto || strcmp(audit_type, "permissions") == 0)
        sa_audit_permissions(target, list);
    if (is_auto || strcmp(audit_type, "network") == 0)
        sa_audit_network(target, list);
}

/* ==================== 去重 / 排序 / 过滤 / 评分 ==================== */

static int sa_sev_rank(const char *sev)
{
    if (strcmp(sev, "critical") == 0) return 0;
    if (strcmp(sev, "high") == 0) return 1;
    if (strcmp(sev, "medium") == 0) return 2;
    if (strcmp(sev, "low") == 0) return 3;
    if (strcmp(sev, "info") == 0) return 4;
    return 5;
}

static void sa_dedup(sa_list_t *list)
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

static void sa_sort(sa_list_t *list)
{
    for (size_t i = 1; i < list->count; i++) {
        sa_finding_t key = list->items[i];
        size_t j = i;
        while (j > 0 &&
               sa_sev_rank(list->items[j - 1].severity) >
                   sa_sev_rank(key.severity)) {
            list->items[j] = list->items[j - 1];
            j--;
        }
        list->items[j] = key;
    }
}

static void sa_filter(sa_list_t *list, const char *threshold)
{
    int min_level = 1; /* 默认 low */
    if (sa_icmp(threshold, "info") == 0) min_level = 0;
    else if (sa_icmp(threshold, "medium") == 0) min_level = 2;
    else if (sa_icmp(threshold, "high") == 0) min_level = 3;
    else if (sa_icmp(threshold, "critical") == 0) min_level = 4;

    size_t w = 0;
    for (size_t i = 0; i < list->count; i++) {
        int lvl = sa_sev_rank(list->items[i].severity);
        if (lvl >= 5) lvl = 4;
        if (4 - lvl >= min_level) {
            if (w != i) list->items[w] = list->items[i];
            w++;
        }
    }
    list->count = w;
}

/* 风险评分：critical×10 + high×6 + medium×3 + low×1 + info×0.5，×2 后 clamp */
static double sa_compute_risk(const sa_list_t *list)
{
    if (list->count == 0) return 0.0;
    double score = 0.0;
    for (size_t i = 0; i < list->count; i++) {
        const char *sev = list->items[i].severity;
        if (strcmp(sev, "critical") == 0) score += 10;
        else if (strcmp(sev, "high") == 0) score += 6;
        else if (strcmp(sev, "medium") == 0) score += 3;
        else if (strcmp(sev, "low") == 0) score += 1;
        else score += 0.5;
    }
    score *= 2.0;
    if (score < 0.0) score = 0.0;
    if (score > 100.0) score = 100.0;
    /* 保留 1 位小数（对应 Python round(x, 1)） */
    score = (double)((long)(score * 10.0 + 0.5)) / 10.0;
    return score;
}

static const char *sa_risk_level(double score)
{
    if (score >= 80.0) return "critical";
    if (score >= 50.0) return "high";
    if (score >= 25.0) return "medium";
    if (score >= 10.0) return "low";
    return "info";
}

static void sa_upper(const char *in, char *out, size_t out_sz)
{
    size_t i = 0;
    for (; in[i] && i + 1 < out_sz; i++)
        out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = '\0';
}

static void sa_make_summary(char *buf, size_t bufsz, const char *audit_type,
                            const sa_list_t *list, double score)
{
    unsigned counts[5] = {0, 0, 0, 0, 0};
    static const char *names[5] = {"critical", "high", "medium",
                                   "low", "info"};
    for (size_t i = 0; i < list->count; i++) {
        int r = sa_sev_rank(list->items[i].severity);
        if (r >= 0 && r < 5) counts[r]++;
    }
    char level_up[16];
    sa_upper(sa_risk_level(score), level_up, sizeof(level_up));
    buf[0] = '\0';
    size_t n = snprintf(buf, bufsz,
                        "Security audit (%s): %s risk (score %.1f/100)",
                        audit_type, level_up, score);
    for (int i = 0; i < 5; i++) {
        if (counts[i] > 0 && n < bufsz) {
            n += snprintf(buf + n, bufsz - n, ", %u %s", counts[i], names[i]);
        }
    }
}

/* ==================== 插件 ABI 实现 ==================== */

const plugin_metadata_t *plugin_get_metadata(void)
{
    return &g_metadata;
}

int plugin_init(const char *config_path, void **user_data)
{
    if (!user_data) return -1;

    sa_user_data_t *ud = (sa_user_data_t *)calloc(1, sizeof(sa_user_data_t));
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
    sa_user_data_t *ud = (sa_user_data_t *)user_data;
    if (!ud) return -1;

    /* 自检：对内置样例执行一次真实审计，验证核心逻辑可用 */
    static const char sample[] =
        "USER root\n"
        "RUN chmod 777 /app/data\n"
        "requests==2.30.0\n";

    sa_list_t list;
    memset(&list, 0, sizeof(list));
    sa_quick_scan(sample, "auto", &list);
    sa_static_audit(sample, "auto", &list);
    if (list.count == 0) return -1;

    ud->running = 1;
    return 0;
}

int plugin_stop(void *user_data)
{
    sa_user_data_t *ud = (sa_user_data_t *)user_data;
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
 * 输入 JSON：{"target": "...", "audit_type": "auto",
 *             "framework": "owasp", "severity_threshold": "low"}
 * 输出 JSON：{"audit_type": "...", "framework": "...",
 *             "risk_score": N, "risk_level": "...",
 *             "findings": [...], "summary": "...",
 *             "recommendations": [...]}
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

    cJSON *target_item = cJSON_GetObjectItem(root, "target");
    if (!cJSON_IsString(target_item)) {
        cJSON_Delete(root);
        return -1;
    }
    const char *target = target_item->valuestring;
    const char *s = target;
    while (*s && sa_isspace_c(*s)) s++;
    if (!*s) { /* 空白目标视为空输入 */
        cJSON_Delete(root);
        return -1;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "audit_type");
    const char *audit_type =
        cJSON_IsString(type_item) ? type_item->valuestring : "auto";
    static const char *valid_types[] = {"config", "dependencies",
                                        "permissions", "network",
                                        "compliance", "auto"};
    int type_ok = 0;
    for (size_t i = 0; i < sizeof(valid_types) / sizeof(valid_types[0]); i++) {
        if (sa_icmp(audit_type, valid_types[i]) == 0) { type_ok = 1; break; }
    }
    if (!type_ok) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *fw_item = cJSON_GetObjectItem(root, "framework");
    const char *framework =
        cJSON_IsString(fw_item) && fw_item->valuestring[0]
            ? fw_item->valuestring
            : "owasp";

    cJSON *thr_item = cJSON_GetObjectItem(root, "severity_threshold");
    const char *threshold = cJSON_IsString(thr_item) &&
                                    thr_item->valuestring[0]
                                ? thr_item->valuestring
                                : "low";

    /* auto → 自动检测审计类型 */
    if (strcmp(audit_type, "auto") == 0)
        audit_type = sa_detect_type(target);

    /* 1) 收集全部 findings */
    sa_list_t list;
    memset(&list, 0, sizeof(list));
    sa_quick_scan(target, audit_type, &list);
    sa_static_audit(target, audit_type, &list);

    /* 2) 去重 → 排序 → 阈值过滤 */
    sa_dedup(&list);
    sa_sort(&list);
    sa_filter(&list, threshold);

    /* 3) 评分与摘要 */
    double risk = sa_compute_risk(&list);
    const char *level = sa_risk_level(risk);
    char summary[512];
    sa_make_summary(summary, sizeof(summary), audit_type, &list, risk);

    /* 4) recommendations：去重的 remediation 列表 */
    char recs[SA_MAX_FINDINGS][SA_REMED_LEN];
    size_t nrecs = 0;
    for (size_t i = 0; i < list.count; i++) {
        const char *r = list.items[i].remediation;
        if (!r[0]) continue;
        int seen = 0;
        for (size_t j = 0; j < nrecs; j++) {
            if (strcmp(recs[j], r) == 0) { seen = 1; break; }
        }
        if (!seen) {
            strncpy(recs[nrecs++], r, SA_REMED_LEN - 1);
            if (nrecs >= SA_MAX_FINDINGS) break;
        }
    }

    /* 5) 输出 JSON */
    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddStringToObject(out, "audit_type", audit_type);
    cJSON_AddStringToObject(out, "framework", framework);
    cJSON_AddNumberToObject(out, "risk_score", risk);
    cJSON_AddStringToObject(out, "risk_level", level);
    cJSON_AddStringToObject(out, "summary", summary);

    cJSON *arr = cJSON_AddArrayToObject(out, "findings");
    for (size_t i = 0; i < list.count; i++) {
        sa_finding_t *f = &list.items[i];
        cJSON *fj = cJSON_CreateObject();
        if (!fj) continue;
        cJSON_AddStringToObject(fj, "id", f->id);
        cJSON_AddStringToObject(fj, "severity", f->severity);
        cJSON_AddStringToObject(fj, "category", f->category);
        cJSON_AddStringToObject(fj, "title", f->title);
        cJSON_AddStringToObject(fj, "description", f->description);
        cJSON_AddStringToObject(fj, "remediation", f->remediation);
        if (f->cve_ref[0])
            cJSON_AddStringToObject(fj, "cve_ref", f->cve_ref);
        cJSON_AddItemToArray(arr, fj);
    }

    cJSON *rec_arr = cJSON_AddArrayToObject(out, "recommendations");
    for (size_t i = 0; i < nrecs; i++) {
        cJSON_AddItemToArray(rec_arr, cJSON_CreateString(recs[i]));
    }

    char *serialized = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!serialized) return -1;

    *json_output = serialized;
    return 0;
}
