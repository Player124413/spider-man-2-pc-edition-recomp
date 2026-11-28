// Меню игры для ввода хоста (см. menu.h). GUI игры — UE2 GUI (UT2003): GUIController (у Shrek 2 —
// ShGUIController в пакете Transient) держит ActivePage; страница — GUIMultiComponent со списком Controls;
// у элемента GUIComponent есть bVisible, bAcceptsInput, MenuState (2 — MSAT_Focused, 4 — MSAT_Disabled) и
// Bounds[4] — левый, верхний, правый, нижний край в пикселях заднего буфера (считаются при отрисовке).
//
// Геймпад в меню — родными средствами GUI. Фокус: событие SetFocus элемента (как у клавиатуры; фокус идёт
// цепочкой GUIMultiComponent.FocusedControl от страницы к элементу, элемент в фокусе подсвечивается стилем
// игры). Соседа в направлении крестовины ищем сами по Bounds: ссылки Links[4], по которым ходит код Xbox
// (UGUIMultiComponent::XControllerEvent), в версии для ПК пусты. Курсор GUI — GUIController.MouseX/MouseY
// (пиксели заднего буфера; мышь двигает их смещениями IK_MouseX/IK_MouseY): при переходе крестовиной курсор
// встаёт на элемент в фокусе и прячется (bHideMouseCursor) — наведение мышью не подсвечивает другой элемент,
// а A — обычный щелчок по нему.
#include "runtime/game/menu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "runtime/game/uobj.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/overlay.h"
#include "runtime/rt.h"

#define MAX_ITEMS 96
#define MAX_CLS 256

static u32 g_ctrl;  // GUIController игры
static u32 g_frames;
static u32 g_cls_epoch;  // эпоха CollectGarbage, для которой актуален кеш g_cls[]
static struct {
    u32 active_page, controls, menu_state, bounds, focused, owner, mouse_x, mouse_y, win_left, win_width;
    uo_prop visible, accepts, hide_cursor, never_focus, force_check;
    int ok;
} g_off;
enum {
    C_MULTI = 1, C_SLIDER = 2, C_COMBO = 4, C_TABS = 8, C_EDIT = 16,
    C_OPT_GAME = 32, C_OPT_VIDEO = 64, C_OPT_PAGE = 128,
};
static struct {
    u32 cls, kind, caption;  // C_*; смещение FString Caption (0 — нет)
} g_cls[MAX_CLS];            // кеш по классам (сбрасывается при каждой CollectGarbage)

typedef struct {
    u32 comp;
    game_rect r;
    int kind;  // GM_*
} item;

// состояние для хоста и запросы от него
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_open, g_focus_kind, g_has_back;
static game_rect g_back;
static int g_req_nav[8], g_req_nav_n, g_req_tab, g_req_cursor = -1, g_req_edit;
static unsigned g_point_req, g_point_done;  // номер последнего запроса точки и исполненного
static float g_point_x, g_point_y;
static u32 g_point_frame;  // кадр, в котором точка поставлена
static int g_el_req, g_el_at, g_el_res;     // запрос «где элемент меню» (для точного нажатия хостом)
static float g_el_x, g_el_y, g_el_cx, g_el_cy;
static int g_moved;        // гость сам поставил курсор (g_moved_x, g_moved_y)
static float g_moved_x, g_moved_y;

static int cls_slot(u32 obj)
{
    u32 cls = uo_class(obj);
    if (!cls)
        return -1;
    for (int i = 0; i < MAX_CLS; i++) {
        if (g_cls[i].cls == cls)
            return i;
        if (!g_cls[i].cls) {
            uo_prop p;
            g_cls[i].cls = cls;
            g_cls[i].kind = (uo_is_a(obj, "GUIMultiComponent") ? C_MULTI : 0) |
                            (uo_is_a(obj, "GUISlider") ? C_SLIDER : 0) |
                            (uo_is_a(obj, "GUIComboBox") ? C_COMBO : 0) |
                            (uo_is_a(obj, "GUITabControl") ? C_TABS : 0) |
                            (uo_is_a(obj, "GUIEditBox") ? C_EDIT : 0) |
                            (uo_is_a(obj, "Tab_OptionsGame") ? C_OPT_GAME : 0) |
                            (uo_is_a(obj, "Tab_OptionsVideo") ? C_OPT_VIDEO : 0) |
                            (uo_is_a(obj, "Tab_Options") ? C_OPT_PAGE : 0);
            g_cls[i].caption = uo_find_prop(cls, "Caption", &p) ? p.offset : 0;
            return i;
        }
    }
    memset(g_cls, 0, sizeof g_cls);
    return cls_slot(obj);
}

static u32 kind_of(u32 obj)
{
    int i = cls_slot(obj);
    return i >= 0 ? g_cls[i].kind : 0;
}

static int is_multi(u32 obj) { return (kind_of(obj) & C_MULTI) != 0; }

// буква в верхний регистр: латиница и кириллица (А-Я, Ё)
static u32 upper16(u32 ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0x430 && ch <= 0x44F))
        return ch - 0x20;
    return ch == 0x451 ? 0x401 : ch;
}

// подпись элемента — кнопка «назад» (BACK, CANCEL, NO; в русской версии НАЗАД, ОТМЕНИТЬ, НЕТ)?
static int is_back(u32 obj)
{
    static const u16 back[] = {'B', 'A', 'C', 'K', 0}, cancel[] = {'C', 'A', 'N', 'C', 'E', 'L', 0}, no[] = {'N', 'O', 0},
                     nazad[] = {0x41D, 0x410, 0x417, 0x410, 0x414, 0}, otmenit[] = {0x41E, 0x422, 0x41C, 0x415, 0x41D, 0x418, 0x422, 0x42C, 0},
                     otmena[] = {0x41E, 0x422, 0x41C, 0x415, 0x41D, 0x410, 0}, net[] = {0x41D, 0x415, 0x422, 0};
    static const u16 *const names[] = {back, cancel, no, nazad, otmenit, otmena, net};
    int i = cls_slot(obj);
    if (i < 0 || !g_cls[i].caption)
        return 0;
    u32 str = obj + g_cls[i].caption, data = rd32(g_mem, str), num = rd32(g_mem, str + 4);
    for (size_t k = 0; data && num && k < sizeof names / sizeof names[0]; k++) {
        u32 j = 0;
        while (names[k][j] && j + 1 < num && upper16(rd16(g_mem, data + 2 * j)) == names[k][j])
            j++;
        if (!names[k][j] && j + 1 == num)
            return 1;
    }
    return 0;
}

static u32 find_controller(void)
{
    u32 cls = uo_find_class("GUIController");
    for (u32 i = 0, n = cls ? uo_count() : 0; i < n; i++) {
        u32 o = uo_at(i);
        if (o && uo_is_a_class(o, cls))
            return o;
    }
    return 0;
}

static int init_offsets(u32 page)
{
    static const struct {
        int ctrl;  // свойство GUIController (иначе — элемента страницы)
        const char *name;
        u32 *off;
    } props[] = {
        {1, "MouseX", &g_off.mouse_x}, {1, "MouseY", &g_off.mouse_y}, {0, "Controls", &g_off.controls},
        {0, "MenuState", &g_off.menu_state}, {0, "Bounds", &g_off.bounds}, {0, "FocusedControl", &g_off.focused},
        {0, "MenuOwner", &g_off.owner},
    };
    uo_prop p;
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (!uo_find_prop(uo_class(props[i].ctrl ? g_ctrl : page), props[i].name, &p))
            return 0;
        *props[i].off = p.offset;
    }
    if (!uo_find_prop(uo_class(page), "bVisible", &g_off.visible) ||
        !uo_find_prop(uo_class(page), "bAcceptsInput", &g_off.accepts) ||
        !uo_find_prop(uo_class(page), "bNeverFocus", &g_off.never_focus) ||
        !uo_find_prop(uo_class(g_ctrl), "bHideMouseCursor", &g_off.hide_cursor) ||
        !uo_find_prop(uo_class(g_ctrl), "bForceMouseCheck", &g_off.force_check))
        return 0;
    if (uo_find_prop(uo_class(page), "WinLeft", &p))
        g_off.win_left = p.offset;
    if (uo_find_prop(uo_class(page), "WinWidth", &p))
        g_off.win_width = p.offset;
    return g_off.ok = 1;
}

static int flag(u32 obj, const uo_prop *p) { return (rd32(g_mem, obj + p->offset) & p->bitmask) != 0; }
static void set_flag(u32 obj, const uo_prop *p, int v)
{
    u32 w = rd32(g_mem, obj + p->offset);
    wr32(g_mem, obj + p->offset, v ? w | p->bitmask : w & ~p->bitmask);
}

static void rect_of(u32 comp, game_rect *r)
{
    r->x0 = rdf32(g_mem, comp + g_off.bounds);
    r->y0 = rdf32(g_mem, comp + g_off.bounds + 4);
    r->x1 = rdf32(g_mem, comp + g_off.bounds + 8);
    r->y1 = rdf32(g_mem, comp + g_off.bounds + 12);
}

// ---------------------------------------------------------------- правки страниц настроек
// Панели вкладок настроек (и в игре, и в главном меню — подклассы): Tab_OptionsGame — клавиши, скорость мыши,
// Reset; Tab_OptionsVideo / Tab_InGameOptionsVideo — яркость, разрешение, глубина цвета, рендерер, детализация.
// Ссылки на элементы — свойства панели (btnKeyForward, cbResolution...). Правки повторяются каждый кадр, пока
// панель видна: игра сама переписывает подписи и видимость при показе вкладки. Признаки C_OPT_GAME/C_OPT_VIDEO
// хранятся в кеше классов g_cls[], который сбрасывается при каждой CollectGarbage (смена карты, новая игра).

static u32 member(u32 panel, const char *name)
{
    uo_prop p;
    return uo_find_prop(uo_class(panel), name, &p) ? rd32(g_mem, panel + p.offset) : 0;
}

static void hide(u32 comp)
{
    if (comp)
        set_flag(comp, &g_off.visible, 0);
}

// вкладка GAME — схема управления геймпадом: подписи строк и значки кнопок Switch (переназначение клавиш выключено).
// Проверка btnHelp (tools/recon/upkg.py по SHGame.u): единственная btnHelp в пакетах — DriveThroughMenu.btnHelp;
// InternalOnClick вызывает PlayHelpDialog() (озвученная подсказка магазина через SaySimpleComment, без внешних
// документов/URL), поэтому btnHelp работает внутри игры и скрывать её не нужно.
static int g_rus;  // русская версия (game_menu_set_language)
static unsigned g_bb_w, g_bb_h;
static int g_game_vis;
static game_rect g_game_btns[6];
static int g_game_btn_ok[6];
static int g_draw_open, g_draw_focus_kind, g_draw_has_tabs;

static const struct {
    const char *label, *button, *caption[2], *keys[2];  // [0] — английская, [1] — русская
} g_rows[] = {
    {"lblKeyForward", "btnKeyForward", {"MOVE", "ДВИЖЕНИЕ"}, {"LEFT STICK", "ЛЕВЫЙ СТИК"}},
    {"lblKeyBackward", "btnKeyBackward", {"CAMERA", "КАМЕРА"}, {"RIGHT STICK", "ПРАВЫЙ СТИК"}},
    {"lblKeyTurnLeft", "btnKeyTurnLeft", {"ACTION", "ДЕЙСТВИЕ"}, {"X", "X"}},
    {"lblKeyTurnRight", "btnKeyTurnRight", {"POTIONS", "ЗЕЛЬЯ"}, {"ZL + D-PAD, X A B Y, R", "ZL + КРЕСТОВИНА, X A B Y, R"}},
    {"lblKeyJump", "btnKeyJump", {"JUMP", "ПРЫЖОК"}, {"B", "B"}},
    {"lblKeyAttack", "btnKeyAttack", {"ATTACK", "УДАР"}, {"A", "A"}},
};

// делегат элемента — пустой (Object и FunctionName — 0): движок зовёт тогда пустую реализацию GUIComponent
static void unbind(u32 comp, const char *delegate)
{
    uo_prop p;
    if (comp && uo_find_prop(uo_class(comp), delegate, &p)) {
        wr32(g_mem, comp + p.offset, 0);
        wr32(g_mem, comp + p.offset + 4, 0);
    }
}

// за край экрана (WinLeft больше 1 — в пикселях) и без ввода/фокуса: видимость элементам панели игра выставляет
// заново каждый кадр (Tab_OptionsVideo.Internal_OnDraw, Tab_OptionsGame.RefreshPage), но WinLeft и делегаты не
// трогает. У составных элементов (GUIComboBox: Edit, MyShowListBtn, MyListBox) Internal_OnDraw возвращает
// bVisible=true родителю уже после PreDraw, из-за чего UGUIComboBox::PreDraw пропускает SetDims дочерних, а Draw
// рисует их на старом месте — поэтому уводим и скрываем рекурсивно все дочерние Controls (их игра не трогает).
static void move_away_rec(u32 comp, int depth)
{
    if (!comp)
        return;
    hide(comp);
    set_flag(comp, &g_off.accepts, 0);
    set_flag(comp, &g_off.never_focus, 1);
    unbind(comp, "__OnClick__Delegate");
    unbind(comp, "__OnChange__Delegate");
    u32 off = g_off.win_left;
    if (!off) {
        uo_prop p;
        if (uo_find_prop(uo_class(comp), "WinLeft", &p))
            off = p.offset;
    }
    if (off)
        wrf32(g_mem, comp + off, 100000.0f);
    if (depth < 3 && is_multi(comp)) {
        u32 arr = rd32(g_mem, comp + g_off.controls), num = rd32(g_mem, comp + g_off.controls + 4);
        for (u32 i = 0; i < num && i < 64; i++) {
            u32 ch = rd32(g_mem, arr + 4 * i);
            if (ch && ch != comp)
                move_away_rec(ch, depth + 1);
        }
    }
}

static void move_away(u32 comp) { move_away_rec(comp, 0); }

static void patch_game(u32 panel, int draw_icons)
{
    if (draw_icons) {
        g_game_vis = 1;
        memset(g_game_btn_ok, 0, sizeof g_game_btn_ok);
    }
    for (size_t i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        uo_set_str(member(panel, g_rows[i].label), "Caption", g_rows[i].caption[g_rus]);
        u32 b = member(panel, g_rows[i].button);
        if (b) {
            // Очищаем текст кнопки — поверх рамки кнопки рисуем значки Switch через overlay.c.
            // Запасной текст в Caption не пишем, иначе на первом кадре показа вкладки он проступает под значками.
            uo_set_str(b, "Caption", "");
            if (draw_icons) {
                game_rect r;
                rect_of(b, &r);
                if (r.x1 - r.x0 >= 8.0f && r.y1 - r.y0 >= 8.0f && r.x0 >= 0.0f && r.y0 >= 0.0f &&
                    r.x1 <= (float)g_bb_w && r.y1 <= (float)g_bb_h) {
                    g_game_btns[i] = r;
                    g_game_btn_ok[i] = 1;
                }
            }
            set_flag(b, &g_off.accepts, 0);
            set_flag(b, &g_off.never_focus, 1);
            // Щелчок (или нажатие «мыши», пока кнопка в фокусе) запускает у панели ожидание клавиши (bPolling), и
            // следующая нажатая кнопка — в том числе геймпада — забирает действие строки: Tab_OptionsGame.ProcessKey
            // стирает его у остальных клавиш (W, Ctrl, Joy2...) и сохраняет в User.ini. Обработчики кнопок клавиш
            // (OnClick панели, OnKeyEvent — TabOptionsInput_OnKeyEvent) снимаем.
            unbind(b, "__OnClick__Delegate");
            unbind(b, "__OnKeyEvent__Delegate");
        }
    }
    uo_set_bool(panel, "bPolling", 0);
    uo_set_str(member(panel, "lblMouseSpeedGame"), "Caption", g_rus ? "СКОРОСТЬ КАМЕРЫ" : "LOOK SPEED");
    move_away(member(panel, "btnReset"));  // сброс клавиш стёр бы и привязки геймпада в User.ini
}

// Отцентрировать оставшийся видимый элемент по ширине панели (нормализованные WinLeft/WinWidth в 0..1)
static void center_x(u32 comp)
{
    if (!comp)
        return;
    u32 off_l = g_off.win_left, off_w = g_off.win_width;
    uo_prop pl, pw;
    if (!off_l && uo_find_prop(uo_class(comp), "WinLeft", &pl))
        off_l = pl.offset;
    if (!off_w && uo_find_prop(uo_class(comp), "WinWidth", &pw))
        off_w = pw.offset;
    if (!off_l || !off_w)
        return;
    float w = rdf32(g_mem, comp + off_w);
    if (w > 0.0f && w < 1.0f)
        wrf32(g_mem, comp + off_l, (1.0f - w) * 0.5f);
}

// Разрешение и глубину цвета задаёт порт (экран Switch), рендерер — только D3D; btnApplyVideoChanges применяет
// только setres (яркость и детализация применяются сразу в TabOptions_OnChange), поэтому вся правая колонка
// убирается, а оставшаяся левая (яркость, детализация текстур и объектов) центрируется по ширине панели.
static void patch_video(u32 panel)
{
    static const char *const hide_names[] = {"lblResolution", "cbResolution", "lblColorDepth", "cbColorDepth",
                                             "lblRenderer", "cbRenderer", "btnApplyVideoChanges"};
    static const char *const keep_names[] = {"lblBrightness", "sldBrightness", "lblTextureDetail", "cbTextureDetail",
                                             "lblObjectDetail", "cbObjectDetail"};
    for (size_t i = 0; i < sizeof hide_names / sizeof hide_names[0]; i++)
        move_away(member(panel, hide_names[i]));
    uo_set_bool(panel, "btnApplyVideoChangesVisible", 0);
    for (size_t i = 0; i < sizeof keep_names / sizeof keep_names[0]; i++)
        center_x(member(panel, keep_names[i]));
}

static void patch(u32 comp, u32 k)
{
    if (k & C_OPT_GAME)
        patch_game(comp, 1);
    else if (k & C_OPT_VIDEO)
        patch_video(comp);
}

// ---------------------------------------------------------------- элементы страницы
static item g_items[MAX_ITEMS];
static int g_n;
static u32 g_tabs;  // видимый GUITabControl страницы
static int g_back_i;
static u32 g_last_edit;  // последнее текстовое поле, для которого открывали экранную клавиатуру

static void add(u32 c, int kind)
{
    if (g_n >= MAX_ITEMS || g_mem[c + g_off.menu_state] == 4)
        return;
    game_rect r;
    rect_of(c, &r);
    if (r.x1 - r.x0 < 2 || r.y1 - r.y0 < 2 || r.x0 < 0 || r.y0 < 0 || r.x1 > (float)g_bb_w || r.y1 > (float)g_bb_h)
        return;
    if (g_back_i < 0 && kind == GM_BUTTON && is_back(c))
        g_back_i = g_n;
    g_items[g_n++] = (item){c, r, kind};
}

// элементы, принимающие ввод, — в g_items (рекурсивно по Controls); выпадающий список — один элемент
static void collect(u32 comp, int depth)
{
    u32 arr = rd32(g_mem, comp + g_off.controls), num = rd32(g_mem, comp + g_off.controls + 4);
    for (u32 i = 0; i < num && i < 256; i++) {
        u32 c = rd32(g_mem, arr + 4 * i);
        if (!c || !flag(c, &g_off.visible))
            continue;
        u32 k = kind_of(c);
        if (k & (C_OPT_GAME | C_OPT_VIDEO))
            patch(c, k);
        if (!flag(c, &g_off.visible))
            continue;
        if (k & C_COMBO) {
            add(c, GM_COMBO);
            continue;
        }
        if ((k & C_TABS) && !g_tabs)
            g_tabs = c;
        if (flag(c, &g_off.accepts)) {
            int kind = (k & C_SLIDER) ? GM_SLIDER
                     : ((k & C_EDIT) && uo_get_bool(c, "bReadOnly") != 1) ? GM_EDIT
                     : GM_BUTTON;
            add(c, kind);
        }
        if (depth < 6 && (k & C_MULTI))
            collect(c, depth + 1);
    }
}

// элемент в фокусе: конец цепочки FocusedControl от страницы, если он среди g_items (или внутри одного из них)
static int focused_item(u32 page)
{
    u32 cur = page;
    for (int d = 0; d < 8 && is_multi(cur); d++) {
        u32 f = rd32(g_mem, cur + g_off.focused);
        if (!f || f == cur || !flag(f, &g_off.visible))
            break;
        cur = f;
    }
    if (cur == page || g_mem[cur + g_off.menu_state] != 2)
        return -1;
    for (u32 c = cur, d = 0; c && d < 4; c = rd32(g_mem, c + g_off.owner), d++)
        for (int i = 0; i < g_n; i++)
            if (g_items[i].comp == c)
                return i;
    return -1;
}

// сосед элемента from в направлении dir (0 вверх, 1 вниз, 2 влево, 3 вправо); from < 0 — первый по порядку
// чтения (сверху вниз, слева направо)
static int neighbor(int from, int dir)
{
    int best = -1;
    float best_cost = 1e30f;
    for (int i = 0; i < g_n; i++) {
        const game_rect *r = &g_items[i].r;
        float cost;
        if (from < 0)
            cost = r->y0 * 4.0f + r->x0;
        else {
            const game_rect *f = &g_items[from].r;
            if (i == from)
                continue;
            float dx = (r->x0 + r->x1 - f->x0 - f->x1) / 2, dy = (r->y0 + r->y1 - f->y0 - f->y1) / 2;
            float along = dir == 0 ? -dy : dir == 1 ? dy : dir == 2 ? -dx : dx;
            float perp = dir < 2 ? dx : dy;
            // перекрытие по поперечной оси — сосед «прямо» в этом направлении
            float lo = dir < 2 ? (r->x0 > f->x0 ? r->x0 : f->x0) : (r->y0 > f->y0 ? r->y0 : f->y0);
            float hi = dir < 2 ? (r->x1 < f->x1 ? r->x1 : f->x1) : (r->y1 < f->y1 ? r->y1 : f->y1);
            perp = perp < 0 ? -perp : perp;
            // целиком дальше края в этом направлении и не слишком вбок (без перекрытия — в конусе ±63°): справа от
            // широкой «Настройки» и от «Выход» нет ничего — фокус остаётся, а не прыгает на ячейку сбоку
            int beyond = dir == 0 ? r->y1 <= f->y0 + 4.0f : dir == 1 ? r->y0 >= f->y1 - 4.0f
                       : dir == 2 ? r->x1 <= f->x0 + 4.0f : r->x0 >= f->x1 - 4.0f;
            if (along < 4.0f || !beyond || (hi <= lo && perp > 2.0f * along))
                continue;
            cost = along + (hi > lo ? 0.0f : 2.0f * perp);
        }
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    return best;
}

// Курсор GUI. Элемент под курсором (ActiveControl, ему достаётся нажатие) GUI ищет заново при движении мыши; после
// клавиши (стрелка у ползунка) там остаётся прежний, и щелчок по «Назад» уходил странице — bForceMouseCheck.
static void set_mouse(float x, float y)
{
    wrf32(g_mem, g_ctrl + g_off.mouse_x, x);
    wrf32(g_mem, g_ctrl + g_off.mouse_y, y);
    set_flag(g_ctrl, &g_off.force_check, 1);
}

static void focus(int i)
{
    u32 none = 0;
    const game_rect *r = &g_items[i].r;
    if (getenv("SNX_LOG_MENU"))
        fprintf(stderr, "[menu] focus %s (%.0f,%.0f)-(%.0f,%.0f) of %d items\n", uo_name(g_items[i].comp), r->x0, r->y0,
                r->x1, r->y1, g_n);
    // кнопки меню ПК (главная страница книги) — bNeverFocus: SetFocus их не трогает; с геймпада фокус им нужен
    set_flag(g_items[i].comp, &g_off.never_focus, 0);
    uo_call(g_items[i].comp, "SetFocus", &none, 1, NULL);
    float x = (r->x0 + r->x1) / 2, y = (r->y0 + r->y1) / 2;
    set_mouse(x, y);
    set_flag(g_ctrl, &g_off.hide_cursor, 1);
    pthread_mutex_lock(&g_lock);
    g_moved = 1;
    g_moved_x = x;
    g_moved_y = y;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- экранная клавиатура и оверлей меню
// Ввод текста в GUIEditBox (на Switch — системная клавиатура swkbdShow; в стандартных меню Shrek 2
// редактируемых GUIEditBox нет, но хук сработает при фокусе на любом нережимном GUIEditBox [проверить на Switch]).
static void open_swkbd(u32 edit)
{
    char init[256] = "";
    uo_prop tp;
    if (uo_find_prop(uo_class(edit), "TextStr", &tp)) {
        u32 data = rd32(g_mem, edit + tp.offset), num = rd32(g_mem, edit + tp.offset + 4);
        if (data && num)
            hle_w2utf8(data, init, sizeof init);
    }
    u32 max_len = uo_get_u32(edit, "MaxWidth");
    if (!max_len || max_len > 255)
        max_len = 64;
    if (getenv("SNX_LOG_MENU"))
        fprintf(stderr, "[menu] edit focus %s (\"%s\", max %u)\n", uo_name(edit), init, max_len);
#ifdef __SWITCH__
    SwkbdConfig kbd;
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        char out[256] = "";
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, init);
        swkbdConfigSetStringLenMax(&kbd, max_len);
        Result rc = swkbdShow(&kbd, out, sizeof out);
        swkbdClose(&kbd);
        if (R_SUCCEEDED(rc)) {
            uo_set_str(edit, "TextStr", out);
            uo_prop cp;
            if (uo_find_prop(uo_class(edit), "CaretPos", &cp)) {
                u32 n = 0;
                for (const u8 *s = (const u8 *)out; *s; s++)
                    if ((*s & 0xC0u) != 0x80u)
                        n++;
                wr32(g_mem, edit + cp.offset, n);
            }
        }
    }
#endif
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float draw_text_shadow(float x, float y, float h, u32 rgba, const char *s)
{
    ov_text(x + 1.0f, y + 1.0f, h, OV_RGBA(15, 12, 8, 210), s);
    return ov_text(x, y, h, rgba, s);
}

// Значки Switch поверх кнопок btnKey* вкладки Tab_OptionsGame: Bounds переводятся из заднего буфера в окно,
// чтобы значки и текст всегда рисовались с квадратными пикселями 1:1 (и в 16:9, и в 4:3)
static void draw_game_key_icons(float dx, float dy, float dw, float dh)
{
    static const ov_button potion_btns[] = {OV_DPAD, OV_X, OV_A, OV_B, OV_Y, OV_R};
    const u32 txt_col = OV_RGBA(255, 245, 220, 255);
    if (!g_bb_w || !g_bb_h || dw <= 0.0f || dh <= 0.0f)
        return;
    float sx = dw / (float)g_bb_w, sy = dh / (float)g_bb_h;
    for (int i = 0; i < 6; i++) {
        if (!g_game_btn_ok[i])
            continue;
        const game_rect *rb = &g_game_btns[i];
        float rx0 = dx + rb->x0 * sx, ry0 = dy + rb->y0 * sy;
        float rx1 = dx + rb->x1 * sx, ry1 = dy + rb->y1 * sy;
        float rw = rx1 - rx0, rh = ry1 - ry0;
        if (rw < 8.0f || rh < 8.0f)
            continue;
        float ih = clampf(rh * 0.72f, 14.0f, 52.0f);
        float th = ih * 0.72f, gap = ih * 0.28f;
        float total = 0.0f;
        if (i == 0 || i == 1) {
            ov_button b = i == 0 ? OV_LSTICK : OV_RSTICK;
            const char *lbl = g_rows[i].keys[g_rus];
            total = ov_icon_width(ih, b) + gap + ov_text_width(th, lbl);
            if (total > rw - 8.0f && total > 1.0f) {
                float s = (rw - 8.0f) / total;
                ih *= s;
                th *= s;
                gap *= s;
                total = ov_icon_width(ih, b) + gap + ov_text_width(th, lbl);
            }
            float x = rx0 + (rw - total) * 0.5f, cy = (ry0 + ry1) * 0.5f;
            x += ov_icon(x, cy - ih * 0.5f, ih, b) + gap;
            draw_text_shadow(x, cy - th * 0.54f, th, txt_col, lbl);
        } else if (i == 3) {
            float pgap = ih * 0.22f, plus_w = ov_text_width(th, "+");
            total = ov_icon_width(ih, OV_ZL) + pgap + plus_w + pgap;
            for (size_t k = 0; k < sizeof potion_btns / sizeof potion_btns[0]; k++)
                total += ov_icon_width(ih, potion_btns[k]) + (k ? pgap : 0.0f);
            if (total > rw - 8.0f && total > 1.0f) {
                float s = (rw - 8.0f) / total;
                ih *= s;
                th *= s;
                pgap *= s;
                plus_w = ov_text_width(th, "+");
                total = ov_icon_width(ih, OV_ZL) + pgap + plus_w + pgap;
                for (size_t k = 0; k < sizeof potion_btns / sizeof potion_btns[0]; k++)
                    total += ov_icon_width(ih, potion_btns[k]) + (k ? pgap : 0.0f);
            }
            float x = rx0 + (rw - total) * 0.5f, cy = (ry0 + ry1) * 0.5f;
            x += ov_icon(x, cy - ih * 0.5f, ih, OV_ZL) + pgap;
            x += draw_text_shadow(x, cy - th * 0.54f, th, txt_col, "+") + pgap;
            for (size_t k = 0; k < sizeof potion_btns / sizeof potion_btns[0]; k++)
                x += ov_icon(x, cy - ih * 0.5f, ih, potion_btns[k]) + pgap;
        } else {
            ov_button b = i == 2 ? OV_X : i == 4 ? OV_B : OV_A;
            float iw = ov_icon_width(ih, b);
            float x = rx0 + (rw - iw) * 0.5f, cy = (ry0 + ry1) * 0.5f;
            ov_icon(x, cy - ih * 0.5f, ih, b);
        }
    }
}

// Контекстная строка подсказок Switch внизу справа окна win_w x win_h: значки и подписи без подложки, как в
// системном интерфейсе Switch (серая полупрозрачная плашка выделялась на чёрных полосах меню, особенно на OLED);
// читаемость поверх картинки игры — за счёт мягкой тени под текстом.
static void draw_legend(int win_w, int win_h, int focus_kind, int has_tabs)
{
    typedef struct {
        ov_button b0, b1;
        int nbtns;
        const char *label;
    } hint;
    hint hs[4];
    int nh = 0;
    if (has_tabs)
        hs[nh++] = (hint){OV_L, OV_R, 2, g_rus ? "Вкладки" : "Tabs"};
    if (focus_kind == GM_SLIDER || focus_kind == GM_COMBO)
        hs[nh++] = (hint){OV_DPAD, OV_DPAD, 1, g_rus ? "Изменить" : "Adjust"};
    if (focus_kind == GM_EDIT)
        hs[nh++] = (hint){OV_A, OV_A, 1, g_rus ? "Ввод" : "Edit"};
    else if (focus_kind != GM_SLIDER)
        hs[nh++] = (hint){OV_A, OV_A, 1, g_rus ? "Выбрать" : "Select"};
    hs[nh++] = (hint){OV_B, OV_B, 1, g_rus ? "Назад" : "Back"};

    float ih = clampf((float)win_h * 0.032f, 16.0f, 44.0f);
    float margin_x = (float)win_w * 0.022f, margin_y = (float)win_h * 0.022f;
    float max_w = (float)win_w - 2.0f * margin_x;

    float th = ih * 0.82f, pad_x = 0.0f, pad_y = 0.0f;
    float gap_i = ih * 0.22f, gap_lbl = ih * 0.32f, gap_item = ih * 0.85f;
    float content_w = 0.0f;
    for (int i = 0; i < nh; i++) {
        if (i)
            content_w += gap_item;
        content_w += ov_icon_width(ih, hs[i].b0);
        if (hs[i].nbtns == 2)
            content_w += gap_i + ov_icon_width(ih, hs[i].b1);
        content_w += gap_lbl + ov_text_width(th, hs[i].label);
    }
    float bw = content_w + 2.0f * pad_x;
    if (bw > max_w && bw > 1.0f) {
        float s = max_w / bw;
        ih *= s;
        th *= s;
        pad_x *= s;
        pad_y *= s;
        gap_i *= s;
        gap_lbl *= s;
        gap_item *= s;
        content_w *= s;
        bw = max_w;
    }
    float bh = ih + 2.0f * pad_y;
    float bx = (float)win_w - margin_x - bw;
    float by = (float)win_h - margin_y - bh;
    float sh = th * 0.07f > 1.0f ? th * 0.07f : 1.0f;  // сдвиг тени

    float x = bx + pad_x, cy = by + bh * 0.5f;
    for (int i = 0; i < nh; i++) {
        if (i)
            x += gap_item;
        x += ov_icon(x, cy - ih * 0.5f, ih, hs[i].b0);
        if (hs[i].nbtns == 2) {
            x += gap_i;
            x += ov_icon(x, cy - ih * 0.5f, ih, hs[i].b1);
        }
        x += gap_lbl;
        float ty = cy - th * 0.54f;
        ov_text(x + sh, ty + sh, th, OV_RGBA(0, 0, 0, 160), hs[i].label);
        x += ov_text(x, ty, th, OV_RGBA(245, 245, 245, 255), hs[i].label);
    }
}

void game_menu_draw_overlay(int win_w, int win_h, float dx, float dy, float dw, float dh)
{
    if (!g_draw_open || win_w <= 0 || win_h <= 0)
        return;
    if (g_game_vis)
        draw_game_key_icons(dx, dy, dw, dh);
    draw_legend(win_w, win_h, g_draw_focus_kind, g_draw_has_tabs);
}

// ---------------------------------------------------------------- нажатие без мыши
// Пункт меню под точкой заднего буфера: самый маленький элемент, в чьи Bounds точка попала (кнопка внутри панели
// меньше панели, ползунок меньше страницы). -1 — под точкой ничего.
static int item_at(float x, float y)
{
    int best = -1;
    float best_area = 0.0f;
    for (int i = 0; i < g_n; i++) {
        const game_rect *r = &g_items[i].r;
        if (x < r->x0 || x > r->x1 || y < r->y0 || y > r->y1)
            continue;
        float area = (r->x1 - r->x0) * (r->y1 - r->y0);
        if (best < 0 || area < best_area) {
            best = i;
            best_area = area;
        }
    }
    return best;
}

// ---------------------------------------------------------------- кадр
void game_menu_frame(unsigned bb_w, unsigned bb_h)
{
    static u32 last_page;
    static int settle;  // кадров до автофокуса новой страницы
    if (!uo_init())
        return;
    g_frames++;
    // При любой сборке мусора (смена карты, новая игра) UClass динамически загруженных страниц настроек
    // (Tab_OptionsGame, Tab_OptionsVideo, Tab_InGameOptionsVideo) могут выгрузиться и перегрузиться по
    // новым адресам — сбрасываем кеш классов целиком и проверяем живость контроллера.
    u32 gc_ep = __atomic_load_n(&rt_gc_epoch, __ATOMIC_RELAXED);
    if (gc_ep != g_cls_epoch) {
        g_cls_epoch = gc_ep;
        memset(g_cls, 0, sizeof g_cls);
        if (g_ctrl && uo_at(rd32(g_mem, g_ctrl + 4)) != g_ctrl) {
            g_ctrl = 0;
            memset(&g_off, 0, sizeof g_off);
        }
    }
    // контроллер ищется один раз (живёт всю игру); раз в 10 с — проверка, что он всё ещё на месте
    if (g_ctrl && g_frames % 600 == 0 && uo_at(rd32(g_mem, g_ctrl + 4)) != g_ctrl) {
        g_ctrl = 0;
        memset(&g_off, 0, sizeof g_off);
        memset(g_cls, 0, sizeof g_cls);
    }
    if (!g_ctrl && g_frames % 120 == 1 && (g_ctrl = find_controller()))
        fprintf(stderr, "[menu] GUI controller %s at %08x\n", uo_name(g_ctrl), g_ctrl);
    u32 page = 0;
    if (g_ctrl) {
        uo_prop p;
        if (!g_off.active_page && uo_find_prop(uo_class(g_ctrl), "ActivePage", &p))
            g_off.active_page = p.offset;
        page = g_off.active_page ? rd32(g_mem, g_ctrl + g_off.active_page) : 0;
    }
    g_n = 0;
    g_tabs = 0;
    g_back_i = -1;
    g_game_vis = 0;
    g_bb_w = bb_w;
    g_bb_h = bb_h;
    if (page && !g_off.ok && !init_offsets(page))
        page = 0;
    if (page) {
        u32 pk = kind_of(page);
        if (pk & C_OPT_PAGE) {
            // Заранее правим скрытые вкладки GameTab/VideoTab (пока перелистывается страница или открыта другая
            // вкладка), чтобы на первом кадре показа вкладки не мелькали «W/S/A/D», «Reset» и «640x480».
            u32 gt = member(page, "GameTab"), vt = member(page, "VideoTab");
            if (gt)
                patch_game(gt, 0);
            if (vt)
                patch_video(vt);
        } else if (pk & (C_OPT_GAME | C_OPT_VIDEO)) {
            patch(page, pk);
        }
        collect(page, 0);
    }
    int fi = page ? focused_item(page) : -1;

    int nav[9], nav_n, tab, cursor, req_edit, point = 0;
    float px = 0, py = 0;
    pthread_mutex_lock(&g_lock);
    nav_n = g_req_nav_n;
    memcpy(nav, g_req_nav, sizeof g_req_nav);
    g_req_nav_n = 0;
    tab = g_req_tab;
    g_req_tab = 0;
    cursor = g_req_cursor;
    g_req_cursor = -1;
    req_edit = g_req_edit;
    g_req_edit = 0;
    if (g_point_req != g_point_done) {
        point = 1;
        px = g_point_x;
        py = g_point_y;
    }
    int el = 0, el_at = 0;
    float ex = 0, ey = 0;
    if (g_el_req) {
        el = 1;
        el_at = g_el_at;
        ex = g_el_x;
        ey = g_el_y;
        g_el_req = 0;
    }
    pthread_mutex_unlock(&g_lock);

    if (page) {
        if (page != last_page)
            settle = 3;  // Bounds новой страницы считаются при её первой отрисовке
        else if (settle && !--settle && fi < 0 && cursor != 1 && g_n)
            nav[nav_n++] = -1;  // сразу фокус на первый элемент, как на консоли
        if (tab && g_tabs)
            uo_call(g_tabs, tab > 0 ? "NextPage" : "PrevPage", NULL, 0, NULL);
        if (tab && getenv("SNX_LOG_MENU"))
            fprintf(stderr, "[menu] tab %+d (%s)\n", tab, g_tabs ? uo_name(g_tabs) : "no tab control");
        for (int k = 0; k < nav_n && !tab; k++) {
            int to = neighbor(nav[k] < 0 ? -1 : fi, nav[k]);
            if (to >= 0 && to != fi)
                focus(fi = to);
        }
        if (cursor >= 0)
            set_flag(g_ctrl, &g_off.hide_cursor, !cursor);
        if (point) {
            set_mouse(px, py);
            set_flag(g_ctrl, &g_off.hide_cursor, 1);
        }
        if (el) {
            // нет фокуса (страница только открылась) — берём первый элемент по порядку чтения, как консоль
            int i = el_at ? item_at(ex, ey) : (fi >= 0 ? fi : neighbor(-1, 1));
            pthread_mutex_lock(&g_lock);
            if (i >= 0) {
                g_el_cx = (g_items[i].r.x0 + g_items[i].r.x1) * 0.5f;
                g_el_cy = (g_items[i].r.y0 + g_items[i].r.y1) * 0.5f;
                g_el_res = 1;
            } else {
                g_el_res = 2;
            }
            pthread_mutex_unlock(&g_lock);
            if (getenv("SNX_LOG_MENU"))
                fprintf(stderr, "[menu] element %s: %s (%.0f,%.0f)\n", el_at ? "at point" : "focused",
                        i >= 0 ? uo_name(g_items[i].comp) : "nothing", (double)g_el_cx, (double)g_el_cy);
        }
        if (fi >= 0 && g_items[fi].kind == GM_EDIT) {
            u32 edit = g_items[fi].comp;
            if (edit != g_last_edit || req_edit) {
                g_last_edit = edit;
                open_swkbd(edit);
            }
        } else {
            g_last_edit = 0;
        }
    } else {
        g_last_edit = 0;
        if (el) {  // меню уже закрылось — нажимать нечего
            pthread_mutex_lock(&g_lock);
            g_el_res = 2;
            pthread_mutex_unlock(&g_lock);
        }
    }
    g_draw_open = page != 0;
    g_draw_focus_kind = fi >= 0 ? g_items[fi].kind : GM_NONE;
    g_draw_has_tabs = g_tabs != 0;
    last_page = page;
    pthread_mutex_lock(&g_lock);
    // Меню считаем открытым, только если на активной странице есть хоть один элемент ввода. После паузы и особенно
    // при старте новой игры игра нет-нет да и оставит ActivePage указателем на уже закрытую (или целиком скрытую)
    // страницу: порт из-за этого оставался в «режиме меню» — стик вёл курсор (то есть в геймплее крутил камеру),
    // хотя игра уже идёт, и получалось «одним стиком и персонаж, и камера». Закрытая страница элементов не отдаёт
    // (`collect` берёт только видимые и принимающие ввод), поэтому состояние снимается само. Три кадра терпим:
    // только что открытая страница может успеть собрать элементы не с первого кадра.
    {
        static int empty_frames;
        int open_now = page != 0;
        if (open_now && g_n == 0) {
            if (empty_frames < 3)
                empty_frames++;
            else {
                open_now = 0;
                if (getenv("SNX_LOG_MENU"))
                    fprintf(stderr, "[menu] page %08x has no items — treat as closed\n", page);
            }
        } else
            empty_frames = 0;
        g_open = open_now;
    }
    g_focus_kind = g_draw_focus_kind;
    g_has_back = g_back_i >= 0;
    if (g_has_back)
        g_back = g_items[g_back_i].r;
    if (point) {
        g_point_done = g_point_req;
        g_point_frame = g_frames;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- для хоста
void game_menu_set_language(const char *lang) { g_rus = lang && !strncasecmp(lang, "rus", 3); }
int game_menu_is_rus(void) { return g_rus; }

void game_menu_edit_activate(void)
{
    pthread_mutex_lock(&g_lock);
    g_req_edit = 1;
    pthread_mutex_unlock(&g_lock);
}

int game_menu_open(int *focus_kind)
{
    pthread_mutex_lock(&g_lock);
    int open = g_open;
    if (focus_kind)
        *focus_kind = g_focus_kind;
    pthread_mutex_unlock(&g_lock);
    return open;
}

int game_menu_back(game_rect *r)
{
    pthread_mutex_lock(&g_lock);
    int has = g_open && g_has_back;
    if (has)
        *r = g_back;
    pthread_mutex_unlock(&g_lock);
    return has;
}

void game_menu_nav(int dir)
{
    pthread_mutex_lock(&g_lock);
    if (g_req_nav_n < 8)
        g_req_nav[g_req_nav_n++] = dir;
    pthread_mutex_unlock(&g_lock);
}

void game_menu_tab(int d)
{
    pthread_mutex_lock(&g_lock);
    g_req_tab = d;
    pthread_mutex_unlock(&g_lock);
}

unsigned game_menu_point(float x, float y)
{
    pthread_mutex_lock(&g_lock);
    unsigned t = ++g_point_req;
    g_point_x = x;
    g_point_y = y;
    pthread_mutex_unlock(&g_lock);
    return t;
}

int game_menu_point_done(unsigned ticket)
{
    pthread_mutex_lock(&g_lock);
    // поставлен, и после этого прошёл ещё кадр: GUI нашёл элемент под курсором
    int done = (int)(g_point_done - ticket) >= 0 && g_frames != g_point_frame;
    pthread_mutex_unlock(&g_lock);
    return done;
}

void game_menu_element(int at_point, float x, float y)
{
    pthread_mutex_lock(&g_lock);
    g_el_at = at_point ? 1 : 0;
    g_el_x = x;
    g_el_y = y;
    g_el_req = 1;
    g_el_res = 0;
    pthread_mutex_unlock(&g_lock);
}

int game_menu_element_result(float *x, float *y)
{
    pthread_mutex_lock(&g_lock);
    int r = g_el_res;
    if (r == 1) {
        if (x)
            *x = g_el_cx;
        if (y)
            *y = g_el_cy;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

void game_menu_cursor(int visible)
{
    pthread_mutex_lock(&g_lock);
    g_req_cursor = visible;
    pthread_mutex_unlock(&g_lock);
}

int game_menu_cursor_moved(float *x, float *y)
{
    pthread_mutex_lock(&g_lock);
    int m = g_moved;
    if (m) {
        *x = g_moved_x;
        *y = g_moved_y;
        g_moved = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return m;
}
