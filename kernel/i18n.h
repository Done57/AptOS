#ifndef APTOS_I18N_H
#define APTOS_I18N_H

/* Runtime language switch for the whole kernel UI.
 *
 * Every user visible string lives in kernel/i18n_str.h; call tr(S_...) to get
 * the one for the active language.  The choice is kept in RAM and mirrored to
 * /LANG.CFG when the root filesystem is writable, so it survives a reboot on
 * an installed system.
 */

#define LANG_EN 0
#define LANG_ZH 1
#define LANG_COUNT 2

typedef enum {
    S_NONE = 0,                 /* "no translation": use the literal title */
#define I18N(id, en, zh) id,
#include "i18n_str.h"
#undef I18N
    S_COUNT
} sid_t;

const char *tr(int id);
const char *tr_en(int id);
const char *tr_zh(int id);

int  i18n_lang(void);
void i18n_set_lang(int lang);
void i18n_toggle(void);

const char *i18n_name(int lang);         /* "English" / "中文" */
const char *i18n_lang_name(void);        /* name of the active language */
int  i18n_parse(const char *s);          /* "en"/"eng"/"english"... -> LANG_* */

void i18n_init(void);

/* Height of one text line: 8px is enough for ASCII, CJK needs its 16px cell. */
int  ui_line_h(void);

#endif
