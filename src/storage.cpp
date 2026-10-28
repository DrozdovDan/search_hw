#include "search/storage.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace search {
// Байты собираются явно, без reinterpret_cast<uint32_t*>. Указатель из файла
// может быть не выровнен на четыре байта: postings имеют шаг всего пять байт.
// Приведение char к unsigned устраняет знаковое расширение байтов >= 128.
uint32_t load_u32(const char* p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return value;
}
uint64_t load_u64(const char* p) {
    return uint64_t(load_u32(p)) | (uint64_t(load_u32(p + 4)) << 32);
}
void write_u32(std::ostream& out, uint32_t value) {
    // Сырые sizeof(Posting) байт записывать нельзя: компилятор может добавить
    // padding, а порядок байтов процессора может отличаться от формата файла.
    std::array<char, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<char>((value >> (8 * i)) & 255);
    out.write(bytes.data(), bytes.size());
}
void write_u64(std::ostream& out, uint64_t value) {
    write_u32(out, static_cast<uint32_t>(value));
    write_u32(out, static_cast<uint32_t>(value >> 32));
}
uint32_t read_u32(std::istream& in) {
    std::array<char, 4> bytes{};
    in.read(bytes.data(), bytes.size());
    check_stream(in, "Чтение uint32");
    return load_u32(bytes.data());
}
uint64_t read_u64(std::istream& in) {
    // Сначала читается младшая половина. Отдельная переменная фиксирует порядок
    // чтений, который нельзя полагать заданным для произвольного выражения с |.
    const uint64_t low = read_u32(in);
    return low | (uint64_t(read_u32(in)) << 32);
}
void check_stream(const std::ios& stream, const std::string& context) {
    if (!stream)
        throw std::runtime_error(context + ": ошибка ввода-вывода или усечённый файл");
}
void write_string(std::ostream& out, std::string_view value) {
    if (value.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Слишком длинная строка");
    write_u32(out, static_cast<uint32_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}
std::string read_string(std::istream& in) {
    const auto size = read_u32(in);
    // Повреждённые четыре байта длины не должны вызвать гигабайтную аллокацию
    // до того, как поток обнаружит конец короткого файла.
    if (size > 64 * 1024 * 1024)
        throw std::runtime_error("Повреждённая длина строки");
    std::string value(size, '\0');
    in.read(value.data(), size);
    check_stream(in, "Чтение строки");
    return value;
}
uint64_t directory_size(const fs::path& directory) {
    // Это сумма логических размеров файлов. Блоки ФС, page cache и временные
    // сегменты после их удаления сюда не входят; это не аналог занятой RAM.
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        if (entry.is_regular_file())
            total += entry.file_size();
    return total;
}
uint64_t peak_rss_kb() {
    // Linux сообщает максимум RSS с начала жизни процесса в КиБ. Это включает
    // служебные аллокации и стеки, которые не видит бюджет аллокатора сегмента.
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("getrusage не выполнен");
    return static_cast<uint64_t>(usage.ru_maxrss);
}
MappedFile::MappedFile(const fs::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("Не удалось открыть " + path.string());
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < 0) {
        ::close(fd);
        throw std::runtime_error("Не удалось определить размер " + path.string());
    }
    size_ = static_cast<uint64_t>(info.st_size);
    // mmap с нулевой длиной недопустим, но пустые файлы корректны для индекса
    // без термов. Представляем такой файл парой nullptr/0 без отображения.
    if (size_ != 0) {
        void* mapping = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            ::close(fd);
            throw std::runtime_error("mmap не выполнен для " + path.string());
        }
        data_ = static_cast<char*>(mapping);
    }
    // Отображение существует независимо от открытого fd. Закрытие дескриптора
    // экономит файловые ресурсы; память освобождается позднее через munmap.
    ::close(fd);
}
MappedFile::~MappedFile() {
    if (data_)
        munmap(data_, size_);
}
const char* MappedFile::at(uint64_t offset, uint64_t length) const {
    // Проверяем length > size - offset вместо offset + length > size: сложение
    // двух чисел из повреждённого файла могло бы переполнить uint64 и пройти проверку.
    if (offset > size_ || length > size_ - offset)
        throw std::runtime_error("Выход за границу файла индекса");
    return data_ ? data_ + offset : nullptr;
}
DocumentStoreWriter::DocumentStoreWriter(const fs::path& directory)
    : offsets_(directory / "documents.offsets", std::ios::binary),
      data_(directory / "documents.data", std::ios::binary) {
    check_stream(offsets_, "Создание смещений документов");
    check_stream(data_, "Создание хранилища документов");
}
void DocumentStoreWriter::append(const Document& document, uint32_t snippet_bytes) {
    // Смещение пишется ДО самой записи: элемент i в offsets указывает на её начало.
    // Четыре строки имеют переменные длины, поэтому вычислять адрес как i * размер
    // документа нельзя; фиксирован лишь восьмибайтовый размер элемента offsets.
    write_u64(offsets_, data_offset_);
    write_string(data_, document.docid);
    write_string(data_, document.title);
    write_string(data_, document.url);
    // Не разрезаем многобайтовый символ UTF-8 на границе сниппета.
    size_t length = std::min<size_t>(snippet_bytes, document.body.size());
    while (length < document.body.size() && length > 0 &&
           (static_cast<unsigned char>(document.body[length]) & 0xc0) == 0x80)
        --length;
    write_string(data_, std::string_view(document.body).substr(0, length));
    // 16 — четыре префикса длины по четыре байта. Явный счётчик соответствует
    // wire-формату и позволяет не сбрасывать файловый буфер вызовами tellp.
    data_offset_ += 16 + document.docid.size() + document.title.size() + document.url.size() + length;
    check_stream(data_, "Запись метаданных");
    check_stream(offsets_, "Запись смещений");
}
void DocumentStoreWriter::finish() {
    // Ошибка записи может проявиться при flush, когда данные реально уходят
    // из буфера C++. Проверяем её до того, как индекс будет опубликован.
    data_.flush();
    offsets_.flush();
    check_stream(data_, "Сохранение метаданных");
    check_stream(offsets_, "Сохранение смещений документов");
    data_.close();
    offsets_.close();
}
DocumentStore::DocumentStore(const fs::path& directory)
    : offsets_(directory / "documents.offsets"), data_(directory / "documents.data") {
    if (offsets_.size() % 8)
        throw std::runtime_error("Повреждены смещения документов");
    if (offsets_.size() && load_u64(offsets_.at(0, 8)) != 0)
        throw std::runtime_error("Первый документ должен начинаться с нулевого смещения");
}
Metadata DocumentStore::get(uint32_t doc_id) const {
    if (doc_id >= size())
        throw std::out_of_range("Несуществующий документ");
    uint64_t offset = load_u64(offsets_.at(uint64_t(doc_id) * 8, 8));
    // Следующий offset ограничивает запись; для последнего документа граница —
    // конец data. Расширение до uint64 выполняется до +1 и умножения на восемь.
    const uint64_t end =
        doc_id + uint64_t{1} < size() ? load_u64(offsets_.at((uint64_t(doc_id) + 1) * 8, 8)) : data_.size();
    if (end < offset || end > data_.size())
        throw std::runtime_error("Повреждены границы документа");
    auto string = [&]() {
        // Каждая длина проверяется относительно ЭТОГО документа, а не только
        // всего файла. Иначе ошибочная длина могла бы захватить соседнюю запись.
        if (end - offset < 4)
            throw std::runtime_error("Повреждены метаданные");
        auto length = load_u32(data_.at(offset, 4));
        offset += 4;
        if (length > end - offset)
            throw std::runtime_error("Повреждена длина метаданных");
        std::string result(data_.at(offset, length), length);
        offset += length;
        return result;
    };
    Metadata result;
    result.docid = string();
    result.title = string();
    result.url = string();
    result.snippet = string();
    // После четырёх строк обязаны попасть точно на границу следующей записи.
    if (offset != end)
        throw std::runtime_error("Лишние байты в метаданных");
    return result;
}
}
