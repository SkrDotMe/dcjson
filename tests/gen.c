/* gen.c - test data generator (not used at runtime).
 *
 *   build/gen KIND MIB [SEED] > file.json
 *
 * KIND: records  array of user-like objects (the common API dump)
 *       numbers  arrays of numbers
 *       strings  objects with long strings, escapes and UTF-8
 *       deep     deeply nested arrays and objects
 *       lines    JSON Lines records
 * The output is valid JSON, pretty-printed for records and deep. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t s = 88172645463325252ULL;
static uint64_t rnd(void)
{
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

static const char *const NAMES[] = {"Matti", "Liisa", "Pekka", "Aino", "Jari", "Sanna", "Mikko",
                                    "Kaisa", "Jussi", "Elina", "Tomi", "Riikka"};
static const char *const CITIES[] = {"Helsinki", "Espoo", "Tampere", "Oulu", "Turku", "Jyväskylä",
                                     "Kuopio", "Lahti", "東京", "München"};

static uint64_t out;
static void put(const char *p) { out += (uint64_t)printf("%s", p); }

static void record(uint64_t id, int pretty, const char *ind)
{
    const char *nl = pretty ? "\n" : "", *sp = pretty ? " " : "";
    char b[1024];
    snprintf(b, sizeof b,
             "%s{%s%s  \"id\":%s%llu,%s%s  \"name\":%s\"%s %s\",%s%s  \"email\":%s\"user%llu@example.com\",%s"
             "%s  \"active\":%s%s,%s%s  \"score\":%s%.3f,%s%s  \"city\":%s\"%s\",%s%s  \"manager\":%snull,%s"
             "%s  \"tags\":%s[\"t%u\",%s\"t%u\"],%s%s  \"address\":%s{\"street\":%s\"Katu %u\",%s\"zip\":%s\"%05u\"}%s%s}",
             ind, nl, ind, sp, (unsigned long long)id, nl, ind, sp, NAMES[rnd() % 12], NAMES[rnd() % 12], nl,
             ind, sp, (unsigned long long)id, nl, ind, sp, rnd() % 2 ? "true" : "false", nl, ind, sp,
             (double)(rnd() % 100000) / 100.0, nl, ind, sp, CITIES[rnd() % 10], nl, ind, sp, nl, ind, sp,
             (unsigned)(rnd() % 50), sp, (unsigned)(rnd() % 50), nl, ind, sp, sp, (unsigned)(rnd() % 200),
             sp, sp, (unsigned)(rnd() % 99999), nl, ind);
    put(b);
}

static void deep(int d, uint64_t lim)
{
    if (d > 40 || out > lim) {
        put("1");
        return;
    }
    if (d % 2) {
        put("[");
        for (int i = 0; i < 3; i++) {
            if (i)
                put(",");
            deep(d + 1, lim);
        }
        put("]");
    } else {
        put("{\"k\":");
        deep(d + 1, lim);
        put(",\"v\":\"x\"}");
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: gen records|numbers|strings|deep|lines MIB [SEED]\n");
        return 2;
    }
    uint64_t lim = (uint64_t)strtoull(argv[2], NULL, 10) << 20;
    if (argc > 3)
        s ^= strtoull(argv[3], NULL, 10) * 0x9e3779b97f4a7c15ULL;
    const char *k = argv[1];
    uint64_t id = 1;
    if (!strcmp(k, "records")) {
        put("[\n");
        while (out < lim) {
            record(id++, 1, "  ");
            put(out < lim ? ",\n" : "\n");
        }
        put("]\n");
    } else if (!strcmp(k, "lines")) {
        while (out < lim) {
            record(id++, 0, "");
            put("\n");
        }
    } else if (!strcmp(k, "numbers")) {
        put("[");
        while (out < lim) {
            printf("[%llu,%.6f,-%u,%ue%u]", (unsigned long long)(rnd() % 1000000000),
                   (double)(rnd() % 1000000) / 7.0, (unsigned)(rnd() % 1000), (unsigned)(rnd() % 9 + 1),
                   (unsigned)(rnd() % 30));
            out += 40;
            put(out < lim ? "," : "");
        }
        put("]\n");
    } else if (!strcmp(k, "strings")) {
        put("[");
        while (out < lim) {
            put("{\"text\":\"");
            int n = (int)(rnd() % 2000) + 100;
            for (int i = 0; i < n; i++) {
                unsigned r = (unsigned)(rnd() % 100);
                put(r < 2 ? "\\n" : r < 3 ? "\\u00e4" : r < 4 ? "ö" : r < 5 ? "\\\"" : r < 20 ? " " : "abcdefgh" + (r % 8));
            }
            put("\",\"n\":1}");
            put(out < lim ? "," : "");
        }
        put("]\n");
    } else if (!strcmp(k, "deep")) {
        put("[");
        while (out < lim) {
            deep(0, lim);
            put(out < lim ? "," : "");
        }
        put("]\n");
    } else {
        fprintf(stderr, "gen: unknown kind %s\n", k);
        return 2;
    }
    return 0;
}
