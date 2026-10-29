#pragma once
#include "common.h"

// Показывает список файлов. cwd — текущая папка (с '/' на конце), меняется по ходу.
// Возвращает true и путь в out, если выбран файл; false — выход из программы.
// status — строка ошибки/сообщения для показа внизу (может быть пустой).
bool browser_run(PadState *pad, char *cwd, size_t cwd_len, char *out, size_t out_len, char *status);
