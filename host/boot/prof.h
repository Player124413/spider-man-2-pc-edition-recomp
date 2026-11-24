// Сэмплирующий профайлер snx_boot. Файл: «base <адрес образа> slide <сдвиг>», затем по строке на выборку
// «g|r pc ret1 ret2 ...» (g — поток гостя, r — поток рендера gl_cs.c).
// Свести: tools/probe/prof.py, tools/probe/profsplit.py.
// Поток-сэмплер раз в 1 мс приостанавливает главный поток гостя и поток рендера и читает их регистры
// (Switch: svcSetThreadActivity/svcGetThreadContext3 на ядре 0; мак: thread_suspend/thread_get_state);
// время — по часам, с ожиданиями. Цепочка кадров — по fp в пределах стека потока.
#pragma once

// path — файл профиля; выборки берутся с after_s по after_s + for_s секунд от старта (for_s <= 0 — до выхода).
// Файл пишется по окончании окна или при выходе (prof_dump). Вызывать до создания потока гостя.
void prof_init(const char *path, double after_s, double for_s);
void prof_guest_thread(void);  // из главного потока гостя: его профилируем
void prof_dump(void);          // записать, если ещё не записан
