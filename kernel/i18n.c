#include "i18n.h"
#include "fs.h"
#include "klog.h"
#include "util.h"

#define LANG_FILE "/LANG.CFG"

static const char *const table[S_COUNT][LANG_COUNT] = {
    [S_NONE] = { "", "" },          /* id 0 占位，其余按文件顺序填 */
#define I18N(id, en, zh) { en, zh },
#include "i18n_str.h"
#undef I18N
};

static int lang = LANG_EN;

int i18n_lang(void)
{
    return lang;
}

const char *tr(int id)
{
    if (id <= S_NONE || id >= S_COUNT)
        return "";
    const char *s = table[id][lang];
    return s ? s : table[id][LANG_EN];
}

const char *tr_en(int id)
{
    if (id <= S_NONE || id >= S_COUNT)
        return "";
    return table[id][LANG_EN];
}

const char *tr_zh(int id)
{
    if (id <= S_NONE || id >= S_COUNT)
        return "";
    return table[id][LANG_ZH];
}

const char *i18n_name(int l)
{
    if (l == LANG_ZH)
        return "\xe4\xb8\xad\xe6\x96\x87";       /* 中文 */
    return "English";
}

const char *i18n_lang_name(void)
{
    return i18n_name(lang);
}

int i18n_parse(const char *s)
{
    if (!s || !*s)
        return -1;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (strncasecmp(s, "zh", 2) == 0 || strcasecmp(s, "chinese") == 0 ||
        strcasecmp(s, "cn") == 0 || strcasecmp(s, "chs") == 0)
        return LANG_ZH;
    if (strncasecmp(s, "en", 2) == 0 || strcasecmp(s, "english") == 0 ||
        strcasecmp(s, "us") == 0)
        return LANG_EN;
    return -1;
}

int ui_line_h(void)
{
    return lang == LANG_ZH ? 16 : 9;
}

static void i18n_save(void)
{
    if (!fs_mounted())
        return;
    fs_write(LANG_FILE, lang == LANG_ZH ? "zh\n" : "en\n", 3);
}

void i18n_set_lang(int l)
{
    if (l != LANG_EN && l != LANG_ZH)
        return;
    if (l == lang)
        return;
    lang = l;
    i18n_save();
    kprintf("[i18n] language switched to %s\n", lang == LANG_ZH ? "Chinese" : "English");
}

void i18n_toggle(void)
{
    i18n_set_lang(lang == LANG_ZH ? LANG_EN : LANG_ZH);
}

void i18n_init(void)
{
    char buf[16];
    int n;

    lang = LANG_EN;
    if (!fs_mounted())
        return;
    n = fs_read(LANG_FILE, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = 0;
    n = i18n_parse(buf);
    if (n >= 0) {
        lang = n;
        kprintf("[i18n] language from %s: %s\n", LANG_FILE, i18n_name(lang));
    }
}
