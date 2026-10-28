#include "search/corpus.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <zlib.h>

namespace search {
namespace {
// Размер блока чтения не связан с размером документа: одна JSONL-строка может
// пересекать много блоков. MaxLine ограничивает память на один повреждённый или
// чрезмерно большой документ, но не ограничивает общий размер входного файла.
constexpr size_t BufferSize = 128 * 1024;
constexpr size_t MaxLine = 64 * 1024 * 1024;
using Callback = std::function<bool(Document&&)>;

// Одинаковый разбор строк применяется к обычным файлам, gzip и записям tar.
// Последняя строка без перевода строки тоже является документом.
class Lines {
  public:
    explicit Lines(const Callback& callback) : callback_(callback) {}
    bool feed(const char* data, size_t size) {
        // line_ содержит хвост предыдущего блока без '\n'. Добавляем только до
        // очередного разделителя и вызываем JSON-парсер после сборки всей строки.
        // Поэтому граница блока внутри UTF-8-символа или JSON escape безопасна.
        while (size) {
            const auto* newline = static_cast<const char*>(memchr(data, '\n', size));
            const size_t count = newline ? static_cast<size_t>(newline - data) : size;
            if (line_.size() + count > MaxLine)
                throw std::runtime_error("JSONL-строка превышает 64 МиБ");
            line_.append(data, count);
            if (newline) {
                // false передаётся вверх через все слои до CorpusReader: лимит
                // первых N документов не заставляет дочитывать многогигабайтный gzip.
                if (!emit())
                    return false;
                data += count + 1;
                size -= count + 1;
            } else
                break;
        }
        return true;
    }
    bool finish() { return line_.empty() || emit(); }

  private:
    bool emit() {
        ++number_;
        // Номер относится к физическим строкам, включая пустые: так сообщение
        // об ошибке указывает на ту же строку, которую увидит редактор файла.
        if (line_.empty() || line_ == "\r") {
            line_.clear();
            return true;
        }
        Document document;
        try {
            const auto value = nlohmann::json::parse(line_);
            // Отсутствующее/null поле трактуем как пустой текст, но неожиданный
            // тип (число вместо строки) оставляем ошибкой парсера типов.
            auto field = [&](const char* name) {
                auto found = value.find(name);
                return found == value.end() || found->is_null() ? std::string{} : found->get<std::string>();
            };
            document.docid = field("docid");
            document.url = field("url");
            document.title = field("title");
            document.headings = field("headings");
            document.body = field("body");
            if (document.docid.empty())
                throw std::runtime_error("Нет docid; требуется документный корпус");
            if (!value.contains("body"))
                throw std::runtime_error("Нет body; passage corpus не поддерживается");
        } catch (const std::exception& error) {
            throw std::runtime_error("JSONL, строка " + std::to_string(number_) + ": " + error.what());
        }
        // Document владеет своими строками. После clear и следующего feed рабочий
        // поток всё ещё может анализировать переданный документ без заимствований.
        line_.clear();
        return callback_(std::move(document));
    }
    const Callback& callback_;
    std::string line_;
    uint64_t number_ = 0;
};

// Возвращает false при досрочной остановке callback. inflate сам проверяет CRC
// и размер gzip; исчерпание входа до Z_STREAM_END считается повреждением.
bool read_payload(std::istream& in, uint64_t size, bool gzip, const Callback& callback) {
    // size — число СЖАТЫХ байт записи tar или байт одиночного файла. Декодер
    // не должен прочитать padding/заголовок следующей записи как продолжение gzip.
    Lines lines(callback);
    std::array<char, BufferSize> input{}, output{};
    if (!gzip) {
        while (size) {
            auto count = std::min<uint64_t>(size, input.size());
            in.read(input.data(), static_cast<std::streamsize>(count));
            check_stream(in, "Чтение JSONL");
            size -= count;
            if (!lines.feed(input.data(), count))
                return false;
        }
        return lines.finish();
    }
    z_stream stream{};
    // 15 + 16 включает gzip-обёртку поверх deflate, в том числе проверку trailer.
    if (inflateInit2(&stream, 15 + 16) != Z_OK)
        throw std::runtime_error("Не удалось создать gzip-декодер");
    // Освобождение необходимо и при лимите документов, и при исключении из JSON
    // или callback. Локальный guard делает все эти выходы одинаково корректными.
    struct Guard {
        z_stream* p;
        ~Guard() { inflateEnd(p); }
    } guard{&stream};
    bool ended = false;
    while (!ended || size || stream.avail_in) {
        // Новый вход подаём только после потребления старого: next_in может
        // указывать внутрь предыдущего блока между двумя вызовами inflate.
        if (!stream.avail_in && size) {
            const auto count = std::min<uint64_t>(size, input.size());
            in.read(input.data(), static_cast<std::streamsize>(count));
            check_stream(in, "Чтение gzip");
            size -= count;
            stream.next_in = reinterpret_cast<Bytef*>(input.data());
            stream.avail_in = static_cast<uInt>(count);
        }
        // Gzip допускает несколько последовательно записанных members.
        if (ended) {
            if (inflateReset2(&stream, 15 + 16) != Z_OK)
                throw std::runtime_error("Ошибка сброса gzip");
            ended = false;
        }
        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = output.size();
        const int status = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = output.size() - stream.avail_out;
        // Даже без нового входного блока декодер может выдать остаток распаковки.
        // Поэтому завершение определяется Z_STREAM_END, а не одним size == 0.
        if (!lines.feed(output.data(), produced))
            return false;
        if (status == Z_STREAM_END)
            ended = true;
        else if (status != Z_OK)
            throw std::runtime_error("Повреждён gzip: " +
                                     std::string(stream.msg ? stream.msg : "truncated stream"));
        if (!produced && !stream.avail_in && !size && !ended)
            break;
    }
    // Досрочный callback уже вернул false выше. Сюда доходим при полном чтении:
    // наличие корректного конца gzip тогда обязательно, иначе это усечение файла.
    if (!ended)
        throw std::runtime_error("Незавершённый gzip");
    return lines.finish();
}
uint64_t octal(const char* data, size_t size) {
    // Поля размера/checksum в используемом tar записаны восьмеричными ASCII-
    // цифрами. Проверка ДО умножения на восемь защищает от переполнения заголовка.
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == '\0' || data[i] == ' ')
            continue;
        if (data[i] < '0' || data[i] > '7' || value > (UINT64_MAX >> 3))
            throw std::runtime_error("Некорректное восьмеричное число tar");
        value = value * 8 + static_cast<unsigned>(data[i] - '0');
    }
    return value;
}
bool read_file(const fs::path& path, const Callback& callback) {
    std::ifstream in(path, std::ios::binary);
    check_stream(in, "Открытие корпуса " + path.string());
    if (path.extension() != ".tar") {
        // Имена файлов корпуса могут не иметь .gz. Сигнатура 1f 8b надёжнее
        // расширения; после просмотра двух байт возвращаем поток к началу.
        char magic[2]{};
        in.read(magic, 2);
        in.clear();
        in.seekg(0);
        return read_payload(in, fs::file_size(path),
                            static_cast<unsigned char>(magic[0]) == 0x1f &&
                                static_cast<unsigned char>(magic[1]) == 0x8b,
                            callback);
    }
    std::array<char, 512> header{};
    while (true) {
        in.read(header.data(), header.size());
        check_stream(in, "Чтение заголовка tar");
        if (std::all_of(header.begin(), header.end(), [](char c) {
                return c == 0;
            }))
            return true;
        // При вычислении checksum сам checksum заменяется восемью пробелами —
        // это правило tar, иначе сумма зависела бы от собственного представления.
        uint64_t checksum = 0;
        for (size_t i = 0; i < header.size(); ++i)
            checksum += i >= 148 && i < 156 ? 32 : static_cast<unsigned char>(header[i]);
        if (checksum != octal(header.data() + 148, 8))
            throw std::runtime_error("Неверная контрольная сумма tar");
        const uint64_t size = octal(header.data() + 124, 12);
        const auto start = in.tellg();
        const char type = header[156];
        if ((type == '0' || type == '\0') && size) {
            char magic[2]{};
            in.read(magic, 2);
            in.clear();
            in.seekg(start);
            if (!read_payload(in, size,
                              static_cast<unsigned char>(magic[0]) == 0x1f &&
                                  static_cast<unsigned char>(magic[1]) == 0x8b,
                              callback))
                return false;
        }
        // Служебные GNU/PAX-записи не извлекаются: имена не нужны для JSONL.
        // Размер полезной части округляется вверх до блока 512 байт. Padding
        // пропускаем отдельно, а не отправляем в JSON/gzip как содержимое файла.
        if (size > uint64_t(INT64_MAX) - 511)
            throw std::runtime_error("Слишком большая запись tar");
        in.seekg(start + static_cast<std::streamoff>((size + 511) / 512 * 512));
        check_stream(in, "Переход к следующей записи tar");
    }
}
}
void CorpusReader::read(const fs::path& source, const std::function<bool(Document&&)>& callback) {
    if (fs::is_directory(source)) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            if (!entry.is_regular_file())
                continue;
            const auto ext = entry.path().extension();
            const auto name = entry.path().filename().string();
            if (ext == ".gz" || ext == ".jsonl" || ext == ".json" || name.starts_with("msmarco_doc_"))
                files.push_back(entry.path());
        }
        // Порядок обхода ФС не гарантирован. Сортировка путей обеспечивает
        // повторяемые внутренние ID при повторной индексации того же каталога.
        std::sort(files.begin(), files.end());
        if (files.empty())
            throw std::runtime_error("В каталоге нет JSONL/gzip файлов корпуса");
        for (const auto& file : files)
            if (!read_file(file, callback))
                break;
    } else
        read_file(source, callback);
}
}
