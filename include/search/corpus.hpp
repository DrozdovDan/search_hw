#pragma once
#include <functional>
#include "search/storage.hpp"

namespace search {
// callback возвращает false, когда достигнут требуемый размер префикса корпуса.
// В памяти находится только текущая строка JSONL и буферы декомпрессора.
class CorpusReader {
  public:
    // source: JSONL, gzip, tar с такими файлами или каталог. Владение Document
    // передаётся callback через rvalue, чтобы следующий буфер чтения не мог
    // перезаписать ещё анализируемый текст. false прекращает чтение всего входа,
    // в том числе оставшихся файлов каталога и непрочитанного хвоста gzip.
    static void read(const fs::path& source, const std::function<bool(Document&&)>& callback);
};
}
