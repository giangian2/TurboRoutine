/*
 * cgdiff -- compare two callgrind profiles function by function.
 *
 * callgrind_annotate has no diff mode, and cg_diff refuses callgrind files
 * (it expects a "command:" header while callgrind writes "cmd:"), so this
 * reads the callgrind output format directly and subtracts the two
 * per-function self-cost tables.
 *
 * Reading the raw file rather than the text callgrind_annotate prints is the
 * whole point: that text is a report meant for humans, with call-count rows,
 * annotated sources and summary lines mixed into the same column layout, and
 * a parser for it breaks the moment a flag changes what gets printed.
 *
 * The format is line based and only a handful of records matter:
 *
 *   positions: line            how many position fields precede the costs
 *   events: Ir Dr Dw ...       which cost column is which
 *   summary: 63960387          totals for the whole run
 *   fl=(227) /path/file.c      name compression: defines id 227...
 *   fl=(227)                   ...and later refers to it
 *   fn=(820) graph_dfs         the function the following costs belong to
 *   36 10                      a cost line: position(s) then one value per event
 *   cfn=(824) traversal_new    a call to another function...
 *   calls=1 -36                ...whose inclusive cost is on the NEXT cost
 *   -36 5782883                line, and must not be counted as self cost
 *
 * Ids live in separate namespaces: fl/fi/fe/cfl/cfi/cfe share the file space,
 * fn/cfn share the function space. Only fn= moves the current function; cfn=
 * names a callee and must not.
 *
 * One deliberate difference from callgrind_annotate: code inlined from a
 * header sits inside an fi=/fe= block, and annotate reports it as its own row
 * ("Graph.h:graph_dfs"). Here it stays with the function it was inlined into,
 * which is what you want when reading a diff. Run totals are identical either
 * way; only the attribution of a few thousand instructions moves.
 *
 * Usage:
 *   cgdiff BEFORE.out AFTER.out [before-name] [after-name] [rows]
 */
#define _POSIX_C_SOURCE 200809L /* getline() */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_ROWS 15
#define NAME_WIDTH 52

/* ---- small growable string table, indexed by the id in "fl=(N) name" ---- */

typedef struct
{
    char** name;
    size_t cap;
} NameTable;

static void* xmalloc(size_t n)
{
    void* p = malloc(n);
    if (!p)
    {
        fputs("cgdiff: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void nt_set(NameTable* t, size_t id, const char* name)
{
    if (id >= t->cap)
    {
        size_t cap = t->cap ? t->cap : 256;
        while (id >= cap)
            cap *= 2;
        char** grown = realloc(t->name, cap * sizeof *grown);
        if (!grown)
        {
            fputs("cgdiff: out of memory\n", stderr);
            exit(1);
        }
        for (size_t i = t->cap; i < cap; i++)
            grown[i] = NULL;
        t->name = grown;
        t->cap  = cap;
    }
    free(t->name[id]);
    t->name[id] = strdup(name);
}

static const char* nt_get(const NameTable* t, size_t id)
{
    return (id < t->cap && t->name[id]) ? t->name[id] : "???";
}

static void nt_free(NameTable* t)
{
    for (size_t i = 0; i < t->cap; i++)
        free(t->name[i]);
    free(t->name);
}

/* ---- hash table "file:function" -> (before, after) --------------------- */

typedef struct
{
    char*    key;
    uint64_t before;
    uint64_t after;
} Entry;

typedef struct
{
    Entry* slot;
    size_t cap; /* power of two */
    size_t count;
} Table;

static uint32_t fnv1a(const char* s)
{
    uint32_t h = 2166136261u;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void tbl_init(Table* t, size_t cap)
{
    t->slot  = xmalloc(cap * sizeof *t->slot);
    t->cap   = cap;
    t->count = 0;
    for (size_t i = 0; i < cap; i++)
        t->slot[i].key = NULL;
}

static Entry* tbl_find(Table* t, const char* key);

static void tbl_grow(Table* t)
{
    Table bigger;
    tbl_init(&bigger, t->cap * 2);
    for (size_t i = 0; i < t->cap; i++)
    {
        if (!t->slot[i].key)
            continue;
        Entry* e = tbl_find(&bigger, t->slot[i].key);
        free(e->key);
        *e = t->slot[i]; /* key ownership moves to the new table */
    }
    free(t->slot);
    *t = bigger;
}

/* Returns the slot for key, creating it (zeroed) if absent. */
static Entry* tbl_find(Table* t, const char* key)
{
    if ((t->count + 1) * 10 > t->cap * 7)
        tbl_grow(t);

    size_t mask = t->cap - 1;
    size_t i    = fnv1a(key) & mask;
    while (t->slot[i].key)
    {
        if (strcmp(t->slot[i].key, key) == 0)
            return &t->slot[i];
        i = (i + 1) & mask;
    }
    t->slot[i].key    = strdup(key);
    t->slot[i].before = 0;
    t->slot[i].after  = 0;
    t->count++;
    return &t->slot[i];
}

static void tbl_free(Table* t)
{
    for (size_t i = 0; i < t->cap; i++)
        free(t->slot[i].key);
    free(t->slot);
}

/* ---- parsing ----------------------------------------------------------- */

/*
 * "fl=(227) /path/file.c" or "fl=(227)". Fills *id, records the name in the
 * table when one is present, and returns the name to use. NULL if the record
 * has no "(N)" part at all (an uncompressed "fn=name", which callgrind emits
 * when --compress-strings=no).
 */
static const char* record_name(const char* arg, NameTable* t)
{
    if (*arg != '(')
        return arg[0] ? arg : NULL;

    char*  end;
    size_t id = strtoul(arg + 1, &end, 10);
    if (*end != ')')
        return NULL;

    const char* rest = end + 1;
    while (*rest == ' ')
        rest++;
    if (*rest)
        nt_set(t, id, rest);
    return nt_get(t, id);
}

/* Number of whitespace separated fields in a header value. */
static int count_fields(const char* s)
{
    int n = 0;
    while (*s)
    {
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s)
            n++;
        while (*s && *s != ' ' && *s != '\t')
            s++;
    }
    return n;
}

/* Index of "Ir" among the event names, or -1. */
static int ir_column(const char* events)
{
    int         i = 0;
    const char* s = events;
    while (*s)
    {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        if (strncmp(s, "Ir", 2) == 0 && (s[2] == ' ' || s[2] == '\t' || s[2] == '\0'))
            return i;
        i++;
        while (*s && *s != ' ' && *s != '\t')
            s++;
    }
    return -1;
}

/*
 * Reads one callgrind file, adding each function's self cost into the given
 * side (offsetof-free: a flag picks the field). Returns the run total.
 */
static uint64_t parse(const char* path, Table* tbl, bool is_after)
{
    FILE* f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "cgdiff: cannot open %s\n", path);
        exit(1);
    }

    NameTable   files = {0}, funcs = {0};
    const char* cur_file       = "???";
    const char* cur_func       = "???";
    int         npos           = 1; /* "positions: line" is the default */
    int         ircol          = 0;
    bool        skip_next_cost = false; /* the line right after calls= */
    uint64_t    total          = 0;

    char*   line = NULL;
    size_t  cap  = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, f)) != -1)
    {
        if (len && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;

        /* A cost line starts with a position: a digit, +, - or *. Every
         * other record starts with a letter. */
        if ((line[0] >= '0' && line[0] <= '9') || line[0] == '+' || line[0] == '-' ||
            line[0] == '*')
        {
            if (skip_next_cost)
            {
                skip_next_cost = false; /* inclusive cost of a callee */
                continue;
            }
            /* Skip the position fields, then take the Ir column. */
            char* s = line;
            for (int i = 0; i < npos; i++)
            {
                while (*s == ' ' || *s == '\t')
                    s++;
                while (*s && *s != ' ' && *s != '\t')
                    s++;
            }
            for (int i = 0; i <= ircol; i++)
            {
                while (*s == ' ' || *s == '\t')
                    s++;
                if (!*s)
                    break;
                if (i == ircol)
                {
                    char key[1024];
                    snprintf(key, sizeof key, "%s:%s", cur_file, cur_func);
                    Entry*   e = tbl_find(tbl, key);
                    uint64_t v = strtoull(s, NULL, 10);
                    if (is_after)
                        e->after += v;
                    else
                        e->before += v;
                }
                while (*s && *s != ' ' && *s != '\t')
                    s++;
            }
            continue;
        }

        char* eq = strchr(line, '=');
        char* co = strchr(line, ':');

        if (eq && (!co || eq < co))
        {
            *eq             = '\0';
            const char* key = line;
            char*       arg = eq + 1;
            while (*arg == ' ')
                arg++;

            if (strcmp(key, "fl") == 0 || strcmp(key, "fi") == 0 || strcmp(key, "fe") == 0)
            {
                const char* n = record_name(arg, &files);
                /* fi/fe switch file for inlined code but the cost still
                 * belongs to the current function, so only fl moves it. */
                if (n && strcmp(key, "fl") == 0)
                    cur_file = n;
            }
            else if (strcmp(key, "cfl") == 0 || strcmp(key, "cfi") == 0 || strcmp(key, "cfe") == 0)
            {
                record_name(arg, &files); /* callee side: definition only */
            }
            else if (strcmp(key, "fn") == 0)
            {
                const char* n = record_name(arg, &funcs);
                if (n)
                    cur_func = n;
            }
            else if (strcmp(key, "cfn") == 0)
            {
                record_name(arg, &funcs); /* names the callee, not the current fn */
            }
            else if (strcmp(key, "calls") == 0)
            {
                skip_next_cost = true;
            }
            /* ob=/cob= live in their own namespace and are not needed here. */
            continue;
        }

        if (co)
        {
            *co       = '\0';
            char* arg = co + 1;
            while (*arg == ' ')
                arg++;

            if (strcmp(line, "positions") == 0)
                npos = count_fields(arg);
            else if (strcmp(line, "events") == 0)
            {
                ircol = ir_column(arg);
                if (ircol < 0)
                {
                    fprintf(stderr, "cgdiff: %s has no Ir event\n", path);
                    exit(1);
                }
            }
            else if (strcmp(line, "summary") == 0 || strcmp(line, "totals") == 0)
            {
                if (total == 0)
                    total = strtoull(arg, NULL, 10);
            }
        }
    }

    free(line);
    nt_free(&files);
    nt_free(&funcs);
    fclose(f);
    return total;
}

/* ---- reporting --------------------------------------------------------- */

/* Writes n into buf with thousands separators. Returns buf. */
static char* commas(uint64_t n, char* buf, size_t size)
{
    char raw[32];
    int  len = snprintf(raw, sizeof raw, "%" PRIu64, n);
    int  out = 0;
    for (int i = 0; i < len && out + 2 < (int)size; i++)
    {
        if (i && (len - i) % 3 == 0)
            buf[out++] = ',';
        buf[out++] = raw[i];
    }
    buf[out] = '\0';
    return buf;
}

static char* signed_commas(int64_t d, char* buf, size_t size)
{
    if (d == 0)
    {
        snprintf(buf, size, "0");
        return buf;
    }
    /* 20 digits + 6 separators + NUL is the widest uint64 rendering, so the
     * sign always fits in the caller's buffer. */
    char body[28];
    commas((uint64_t)(d < 0 ? -d : d), body, sizeof body);
    snprintf(buf, size, "%c%s", d < 0 ? '-' : '+', body);
    return buf;
}

/* "/path/src/star.c:star_iter_next" -> "star.c:star_iter_next", truncated. */
static const char* short_name(const char* name, char* buf, size_t size)
{
    const char* slash = strrchr(name, '/');
    if (slash)
        name = slash + 1;
    size_t len = strlen(name);
    if (len <= NAME_WIDTH)
        return name;
    snprintf(buf, size, "...%s", name + len - (NAME_WIDTH - 3));
    return buf;
}

typedef struct
{
    const char* name;
    uint64_t    before;
    uint64_t    after;
    int64_t     delta;
} Row;

static int by_abs_delta(const void* a, const void* b)
{
    int64_t x = ((const Row*)a)->delta, y = ((const Row*)b)->delta;
    if (x < 0)
        x = -x;
    if (y < 0)
        y = -y;
    return (x < y) - (x > y); /* descending */
}

int main(int argc, char** argv)
{
    if (argc < 3 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        fputs("usage: cgdiff BEFORE.out AFTER.out [before-name] [after-name] [rows]\n"
              "\n"
              "Compares two callgrind profiles and reports, per function, how many\n"
              "instructions (Ir) the second run executed compared with the first.\n",
              stderr);
        return argc < 3 ? 2 : 0;
    }

    const char* before_name = argc > 3 ? argv[3] : "before";
    const char* after_name  = argc > 4 ? argv[4] : "after";
    int         rows_wanted = argc > 5 ? atoi(argv[5]) : DEFAULT_ROWS;
    if (rows_wanted <= 0)
        rows_wanted = DEFAULT_ROWS;

    Table tbl;
    tbl_init(&tbl, 4096);
    uint64_t tb = parse(argv[1], &tbl, false);
    uint64_t ta = parse(argv[2], &tbl, true);

    printf("-- %s -> %s: change in instructions executed --\n", before_name, after_name);
    if (tb)
    {
        int64_t delta = (int64_t)ta - (int64_t)tb;
        char    b1[32], b2[32], b3[32];
        printf("   total: %s -> %s   (%s, %+.1f%%)  %s\n", commas(tb, b1, sizeof b1),
               commas(ta, b2, sizeof b2), signed_commas(delta, b3, sizeof b3),
               100.0 * (double)delta / (double)tb,
               delta < 0 ? "better" : (delta == 0 ? "unchanged" : "worse"));
    }
    putchar('\n');

    Row*   rows = xmalloc(tbl.count * sizeof *rows);
    size_t n    = 0;
    for (size_t i = 0; i < tbl.cap; i++)
    {
        Entry* e = &tbl.slot[i];
        if (!e->key || e->before == e->after)
            continue;
        rows[n].name   = e->key;
        rows[n].before = e->before;
        rows[n].after  = e->after;
        rows[n].delta  = (int64_t)e->after - (int64_t)e->before;
        n++;
    }

    if (n == 0)
    {
        puts("   no per-function differences.");
    }
    else
    {
        qsort(rows, n, sizeof *rows, by_abs_delta);

        printf("   %-*s %14s %14s %14s\n", NAME_WIDTH, "function", "before", "after", "delta");
        printf("   %.*s %.14s %.14s %.14s\n", NAME_WIDTH,
               "----------------------------------------------------------------",
               "----------------", "----------------", "----------------");

        size_t shown = n < (size_t)rows_wanted ? n : (size_t)rows_wanted;
        for (size_t i = 0; i < shown; i++)
        {
            char b1[32], b2[32], b3[32], nm[256];
            printf("   %-*s %14s %14s %14s\n", NAME_WIDTH, short_name(rows[i].name, nm, sizeof nm),
                   commas(rows[i].before, b1, sizeof b1), commas(rows[i].after, b2, sizeof b2),
                   signed_commas(rows[i].delta, b3, sizeof b3));
        }
        if (n > shown)
            printf("   ... and %zu more functions (raise the row count)\n", n - shown);
    }

    free(rows);
    tbl_free(&tbl);
    return 0;
}
