#ifndef APTOS_APP_I18N_H
#define APTOS_APP_I18N_H

/* Tiny bilingual helper for ring 3 applications.
 *
 * The kernel owns the language (F1, the start menu or the terminal's `lang`
 * command switch it); applications ask for it once with sys_lang() and pick
 * their strings with the T() macro:
 *
 *     text(60, 92, T("Setup will:", "安装程序会:"), C_TEXT, 1);
 *
 * Keep the Chinese text narrow: a CJK glyph is 16px wide at scale 1, so it
 * needs twice the room of an ASCII character.
 */
#include "aptos.h"

#define APP_LANG_EN 0
#define APP_LANG_ZH 1

static int app_lang_cache = -1;

static inline int app_lang(void)
{
    if (app_lang_cache < 0)
        app_lang_cache = sys_lang();
    return app_lang_cache;
}

static inline void app_lang_refresh(void)
{
    app_lang_cache = sys_lang();
}

#define T(en, zh) (app_lang() == APP_LANG_ZH ? (zh) : (en))

#endif
