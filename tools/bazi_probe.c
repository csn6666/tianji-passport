// tools/bazi_probe.c —— 在主机上跑端侧引擎,吐出与 tools/bazi_ref.py 同格式的 JSON,
// 供 verify_bazi.sh 逐字段对拍。不参与固件编译。
#include "bazi_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jstr(const char *k, const char *v, int comma) {
    printf("\"%s\": \"%s\"%s", k, v, comma ? ", " : "");
}

int main(int argc, char **argv) {
    int ny = 2026, nmo = 8, nd = 26, nh = 12, nmi = 0;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--now"))
            sscanf(argv[i + 1], "%d-%d-%d %d:%d", &ny, &nmo, &nd, &nh, &nmi);
    }

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        bz_birth_t b = { 0 };
        int hu, male;
        if (sscanf(line, "%d %d %d %d %d %d %d %d", &b.year, &b.month, &b.day,
                   &b.hour, &b.minute, &hu, &male, &b.lng100) != 8) continue;
        b.hour_unknown = hu;
        b.male = male;

        char raw[256];
        snprintf(raw, sizeof(raw), "%s", line);
        raw[strcspn(raw, "\r\n")] = 0;

        bz_chart_t c;
        if (!bz_compute(&b, ny, nmo, nd, nh, nmi, &c)) {
            printf("{\"case\": \"%s\", \"error\": \"out of range\"}\n", raw);
            continue;
        }

        // 键名与 bazi_ref.py 一致,且按字母序输出,便于 diff
        printf("{");
        jstr("case", raw, 1);
        printf("\"color_name\": \"%s\", ", c.color_name);
        printf("\"current_dayun\": \"%s\", ", c.current_dayun);
        printf("\"day_master\": \"%s\", ", c.day_master);
        printf("\"dayun\": [");
        for (int i = 0; i < c.dayun_n; i++)
            printf("%s[\"%s\", %d, %d, %d]", i ? ", " : "", c.dayun[i].gan_zhi,
                   c.dayun[i].start_age, c.dayun[i].start_year, c.dayun[i].end_year);
        printf("], ");
        printf("\"direction\": \"%s\", ", c.direction);
        jstr("eight_char", c.eight_char, 1);
        printf("\"favorable\": \"%s\", ", (const char *[]){ "金", "木", "水", "火", "土" }[c.fav]);
        printf("\"is_forward\": %s, ", c.is_forward ? "true" : "false");
        printf("\"ji\": [\"%s\", \"%s\"], ", c.ji[0], c.ji[1]);
        jstr("liunian", c.liunian, 1);
        jstr("liuri", c.liuri, 1);
        jstr("liuri_relation", c.liuri_relation, 1);
        jstr("liuyue", c.liuyue, 1);
        jstr("lunar_date", c.lunar_date, 1);
        jstr("next_jie_date", c.next_jie_date, 1);
        jstr("next_jie_name", c.next_jie_name, 1);
        jstr("next_liuyue", c.next_liuyue, 1);
        printf("\"number\": \"%s\", ", c.number);
        printf("\"pillars\": [\"%s\", \"%s\", \"%s\", \"%s\"], ",
               c.pillar[0], c.pillar[1], c.pillar[2], c.pillar[3]);
        printf("\"score\": %d, ", c.score);
        jstr("solar_date", c.solar_date, 1);
        jstr("start_info", c.start_info, 1);
        printf("\"strong\": %s, ", c.strong ? "true" : "false");
        jstr("true_solar", c.true_solar, 1);
        printf("\"wx_pct\": [%.1f, %.1f, %.1f, %.1f, %.1f], ",
               c.wx_pct[0], c.wx_pct[1], c.wx_pct[2], c.wx_pct[3], c.wx_pct[4]);
        printf("\"yi\": [\"%s\", \"%s\"]", c.yi[0], c.yi[1]);
        printf("}\n");
    }
    return 0;
}
