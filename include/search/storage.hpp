#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace search {
namespace fs = std::filesystem;

// Формат всегда little-endian: файлы не зависят от выравнивания структур C++.
// load читает байты из уже проверенного диапазона памяти; read проверяет поток.
// Это разделение позволяет использовать один формат и для mmap, и для ifstream.
uint32_t load_u32(const char* data);
uint64_t load_u64(const char* data);
void write_u32(std::ostream& out, uint32_t value);
void write_u64(std::ostream& out, uint64_t value);
uint32_t read_u32(std::istream& in);
uint64_t read_u64(std::istream& in);
// Строка хранится как u32 длина + ровно столько байт, без завершающего нуля.
// Длина считается в байтах UTF-8; это не количество отображаемых символов.
void write_string(std::ostream& out, std::string_view value);
std::string read_string(std::istream& in);
void check_stream(const std::ios& stream, const std::string& context);
uint64_t directory_size(const fs::path& directory);
uint64_t peak_rss_kb();

// mmap резервирует виртуальные адреса; ядро подгружает только затронутые страницы.
// Владение отображением разделяется итераторами через shared_ptr уровня индекса.
class MappedFile {
  public:
    explicit MappedFile(const fs::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    const char* data() const { return data_; }
    uint64_t size() const { return size_; }
    // Проверяет весь диапазон до получения указателя. Чтение самого указателя
    // выполняется вызывающим кодом; при пустом файле at(0, 0) возвращает nullptr.
    const char* at(uint64_t offset, uint64_t length) const;

  private:
    char* data_ = nullptr;
    uint64_t size_ = 0;
};

// Полный текущий документ живёт только на стадиях чтения/анализа. Все body
// корпуса одновременно в этих структурах не накапливаются: очередь ограничена.
struct Document {
    std::string docid, url, title, headings, body;
};
// Данные для выдачи отделены от текста для индексирования. Это позволяет после
// поиска прочитать несколько заголовков/URL по ID, не открывая исходный gzip.
struct Metadata {
    std::string docid, title, url, snippet;
};
// Порядок append задаёт внутренний ID: первая запись — 0, следующая — 1 и т.д.
// documents.offsets содержит позицию начала каждой записи в documents.data.
class DocumentStoreWriter {
  public:
    explicit DocumentStoreWriter(const fs::path& directory);
    void append(const Document& document, uint32_t snippet_bytes);
    void finish();

  private:
    std::ofstream offsets_, data_;
    // Явный счётчик сохраняет буферизацию записи: tellp для каждого документа не нужен.
    uint64_t data_offset_ = 0;
};
class DocumentStore {
  public:
    explicit DocumentStore(const fs::path& directory);
    // Адрес смещения вычисляется как doc_id * 8: поиск среди всех документов
    // не требуется. После этого читаются только четыре строки данной записи.
    Metadata get(uint32_t doc_id) const;
    uint64_t size() const { return offsets_.size() / 8; }

  private:
    MappedFile offsets_, data_;
};
}
