#pragma once
#include <memory>
#include <optional>
#include <vector>
#include "search/analyzer.hpp"

namespace search {
// Один терм даёт одну такую запись на документ, независимо от числа повторений
// слова. fields хранит объединение полей. В RAM структура может иметь padding;
// дисковый формат записывается вручную и всегда занимает ровно пять байт.
struct Posting {
    uint32_t doc_id;
    uint8_t fields;
};
// Счётчики относятся к вызовам интерфейса, включая составные итераторы запроса.
// comparisons отдельно учитывает сравнения внутри переходов листовых списков.
// Если метрики не нужны, во все фабрики можно передавать nullptr.
struct IteratorMetrics {
    uint64_t next_calls = 0, advance_calls = 0, comparisons = 0;
};
enum class AdvanceMode { Sequential, Binary };

// Курсор по строго возрастающему потоку doc_id. Его источник может быть файлом,
// вектором или результатом AND/OR/ANDNOT: следующий уровень не различает эти случаи.
// Курсор уже указывает на первый результат сразу после создания, если он есть.
class PostingIterator {
  public:
    // После исчерпания valid остаётся false; next и advance не возвращают курсор назад.
    virtual bool valid() const = 0;
    // Значение разрешено читать только при valid() == true; иначе реализации бросают исключение.
    virtual uint32_t doc_id() const = 0;
    virtual void next() = 0;
    // Оставить текущий результат, если он уже >= target, иначе найти первый такой
    // результат впереди. Если его нет, перейти в исчерпанное состояние.
    virtual void advance(uint32_t target) = 0;
    virtual ~PostingIterator() = default;
};
// Для тестирования операций и отдельного измерения пересечений. Основной поиск
// использует тот же алгоритм перехода поверх дискового отображения postings.
std::unique_ptr<PostingIterator> vector_iterator(std::vector<uint32_t> docs,
                                                 AdvanceMode mode = AdvanceMode::Binary,
                                                 IteratorMetrics* metrics = nullptr);

// offset измеряется в байтах postings.data; df — число документов, а не число
// употреблений слова. Поэтому df одновременно задаёт длину списка в записях.
struct TermInfo {
    uint64_t offset = 0, df = 0;
};
// Словарь упорядочен по байтам UTF-8. Отдельный массив восьмибайтовых смещений
// позволяет обращаться к середине словаря, хотя сами строки имеют разную длину.
// Оба файла отображены в адресное пространство; копии всех термов в heap нет.
class TermDictionary {
  public:
    explicit TermDictionary(const fs::path& directory);
    // Отсутствующий терм возвращает nullopt и впоследствии даёт пустой итератор.
    std::optional<TermInfo> find(std::string_view term) const;
    // ordinal — номер в отсортированном словаре, не doc_id. Возвращаемая строка
    // заимствует память mmap и действительна только пока жив этот словарь.
    std::pair<std::string_view, TermInfo> at(uint64_t ordinal) const;
    uint64_t size() const { return offsets_.size() / 8; }

  private:
    MappedFile offsets_, data_;
};
// Неизменяемый индекс для чтения: словарь, postings и метаданные открываются
// совместно, но читаются по необходимости. Конструктор проверяет манифест и
// размеры; полное перечисление записей выполняет отдельный validate_index.
class DiskIndex {
  public:
    explicit DiskIndex(const fs::path& directory);
    const TermDictionary& dictionary() const { return dictionary_; }
    // term уже должен быть нормализован TextAnalyzer. Any означает все поля;
    // ненулевая маска оставляет документы, где совпал хотя бы один её бит.
    // Итератор удерживает собственное shared-владение файлом postings.
    std::unique_ptr<PostingIterator> postings(std::string_view term, uint8_t field = Any,
                                              AdvanceMode mode = AdvanceMode::Binary,
                                              IteratorMetrics* metrics = nullptr) const;
    const DocumentStore& documents() const { return documents_; }
    uint64_t posting_count() const { return postings_->size() / 5; }

  private:
    TermDictionary dictionary_;
    std::shared_ptr<MappedFile> postings_;
    DocumentStore documents_;
};
struct BuildOptions {
    fs::path corpus, output;
    // Нулевые лимиты max_docs/segment_docs отключены. Первый ограничивает весь
    // вход, второй лишь принудительно дробит сегменты, например для тестов.
    uint64_t max_docs = 0;
    // Размер в МиБ: 75% получает аллокатор сегмента, остальное — резерв.
    // Это бюджет структур, а не ограничение RSS через средства ОС.
    uint64_t memory_limit_mb = 512;
    // Число анализаторов; кроме них существуют поток чтения и основной поток.
    uint32_t threads = 1;
    // Максимум входных сегментов одной группы слияния, не общее их число.
    uint32_t merge_fan_in = 16;
    // Ноль исключает body из метаданных. Иначе хранится только короткий префикс.
    uint32_t snippet_bytes = 0;
    uint32_t segment_docs = 0;
};
struct BuildStats {
    uint64_t documents = 0, terms = 0, postings = 0, segments = 0;
    // RSS измеряется для всего процесса, tracked — только для PMR-структур.
    // Эти показатели нельзя подменять друг другом при оценке лимита памяти.
    uint64_t index_bytes = 0, peak_rss_kb = 0, max_tracked_bytes = 0;
    double seconds = 0;
};
class IndexBuilder {
  public:
    // Создаёт новый индекс и публикует его после успешного слияния. Существующий
    // output отклоняется. Обычное исключение удаляет собственный рабочий каталог;
    // исходный корпус остаётся входом только для чтения.
    static BuildStats build(const BuildOptions& options);
};
// Полная последовательная проверка вынесена из открытия индекса: обычный поиск
// не должен прочитывать все posting lists, чтобы вернуть первые десять ответов.
void validate_index(const fs::path& directory);
}
