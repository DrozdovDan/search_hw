#include "search/index.hpp"
#include "search/corpus.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <memory_resource>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unistd.h>

namespace search {
namespace {
using Clock = std::chrono::steady_clock;
// В файле запись = u32 doc_id + u8 маска. sizeof(Posting) здесь неприменим:
// padding в памяти и порядок байтов процессора не являются частью формата.
constexpr uint64_t PostingBytes = 5;

// Алгоритм перехода общий для тестовых и дисковых итераторов. Экспоненциальный
// поиск сначала ограничивает интервал около текущей позиции, затем бинарный
// находит lower_bound. Малый скачок не затрагивает далёкие дисковые страницы.
// Позиция никогда не уменьшается, в том числе при target <= текущего doc_id.
// get(i) читает ID с позиции i и позволяет повторно использовать алгоритм для
// RAM-вектора и mmap-файла. Функция возвращает позицию, а size означает конец.
// Для прыжка на d записей sequential требует O(d+1) сравнений, binary — O(1+log(d+1)).
// Это число сравнений, не гарантия такого же времени дискового ввода.
template <class Get>
uint64_t advance_position(uint64_t position, uint64_t size, uint32_t target, AdvanceMode mode, Get get,
                          IteratorMetrics* metrics) {
    auto less = [&](uint64_t i) {
        if (metrics)
            ++metrics->comparisons;
        return get(i) < target;
    };
    // Проверка конца идёт первой: get(size) всегда был бы выходом за список.
    // Повторный advance с меньшей целью не должен отменять уже сделанный прогресс.
    if (position == size || !less(position))
        return position;
    if (mode == AdvanceMode::Sequential) {
        do {
            ++position;
        } while (position < size && less(position));
        return position;
    }
    uint64_t low = position + 1, step = 1;
    // Проверяем расстояния 1, 2, 4, 8, ... от текущей позиции. При каждом значении
    // ниже target отбрасываем весь просмотренный префикс, а не одну запись.
    while (step < size - position && less(position + step)) {
        low = position + step + 1;
        // До удвоения проверяем границу: нельзя переполнить step или выйти за size.
        if (step > (size - position) / 2) {
            step = size - position;
            break;
        }
        step *= 2;
    }
    uint64_t high = std::min(size, position + step + 1);
    // Полуинтервал [low, high) содержит искомый lower_bound. Проверенную верхнюю
    // позицию включаем через +1: она сама может оказаться первым ID >= target.
    // При отсутствии ответа low дойдёт до size, что является корректным концом.
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (less(middle))
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}
// Векторный источник нужен эталонным проверкам/микробенчмарку, а не загрузке
// реальных дисковых списков целиком. Все способы перехода у него те же, что у mmap.
class VectorIterator final : public PostingIterator {
  public:
    VectorIterator(std::vector<uint32_t> docs, AdvanceMode mode, IteratorMetrics* metrics)
        : docs_(std::move(docs)), mode_(mode), metrics_(metrics) {
        if (!std::is_sorted(docs_.begin(), docs_.end()) ||
            std::adjacent_find(docs_.begin(), docs_.end()) != docs_.end())
            throw std::invalid_argument("Posting list должен строго возрастать");
    }
    bool valid() const override { return position_ < docs_.size(); }
    uint32_t doc_id() const override {
        if (!valid())
            throw std::out_of_range("Итератор исчерпан");
        return docs_[position_];
    }
    void next() override {
        if (metrics_)
            ++metrics_->next_calls;
        if (valid())
            ++position_;
    }
    void advance(uint32_t target) override {
        if (metrics_)
            ++metrics_->advance_calls;
        position_ = advance_position(
            position_, docs_.size(), target, mode_,
            [&](uint64_t i) {
                return docs_[i];
            },
            metrics_);
    }

  private:
    std::vector<uint32_t> docs_;
    uint64_t position_ = 0;
    AdvanceMode mode_;
    IteratorMetrics* metrics_;
};
// data_ указывает только на диапазон одного терма. position_ — номер записи,
// а не байтовое смещение; файл удерживается shared_ptr до смерти последнего курсора.
class DiskPostingIterator final : public PostingIterator {
  public:
    DiskPostingIterator(std::shared_ptr<MappedFile> file, TermInfo info, uint8_t field, AdvanceMode mode,
                        IteratorMetrics* metrics)
        : file_(std::move(file)), size_(info.df), field_(field), mode_(mode), metrics_(metrics) {
        // Проверяем df ДО умножения на размер записи; затем at проверяет offset
        // вместе с длиной. Так повреждённый словарь не оборачивает арифметику диапазона.
        if (info.df > file_->size() / PostingBytes)
            throw std::runtime_error("Повреждена df");
        data_ = file_->at(info.offset, info.df * PostingBytes);
        skip_fields();
    }
    bool valid() const override { return position_ < size_; }
    uint32_t doc_id() const override {
        if (!valid())
            throw std::out_of_range("Итератор исчерпан");
        return get(position_);
    }
    void next() override {
        if (metrics_)
            ++metrics_->next_calls;
        if (valid())
            ++position_;
        skip_fields();
    }
    void advance(uint32_t target) override {
        if (metrics_)
            ++metrics_->advance_calls;
        position_ = advance_position(
            position_, size_, target, mode_,
            [&](uint64_t i) {
                return get(i);
            },
            metrics_);
        // lower_bound ищет по общему списку. После него фильтр полей может
        // продвинуть позицию дальше, но найденный ID всё равно будет >= target.
        skip_fields();
    }

  private:
    uint32_t get(uint64_t i) const { return load_u32(data_ + i * PostingBytes); }
    void skip_fields() {
        // Any=0 отключает фильтр. Для ненулевой маски достаточно одного общего
        // бита: запись title|body подходит и запросу title, и запросу body.
        // Отдельного skip-индекса по полям пока нет, этот просмотр линейный.
        if (field_)
            while (valid() && !(static_cast<uint8_t>(data_[position_ * PostingBytes + 4]) & field_))
                ++position_;
    }
    std::shared_ptr<MappedFile> file_;
    const char* data_ = nullptr;
    uint64_t position_ = 0, size_;
    uint8_t field_;
    AdvanceMode mode_;
    IteratorMetrics* metrics_;
};

// Учитываются реальные запросы аллокатора хеш-таблицы, строк и векторов сегмента.
// Предварительная оценка позволяет сбросить сегмент до вставки очередного
// документа; сам ресурс служит последней защитой от превышения квоты.
// При любой ошибке весь незавершённый индекс остаётся неопубликованным.
// Ограничение относится к структурам сегмента; буферы JSON, анализа, потоков
// и ОС описаны отдельно в README. Это не ограничение всего RSS процесса.
class BudgetResource final : public std::pmr::memory_resource {
  public:
    explicit BudgetResource(uint64_t limit) : limit_(limit) {}
    uint64_t used = 0, peak = 0;
    uint64_t limit() const { return limit_; }

  private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (bytes > limit_ - used)
            throw std::runtime_error("Превышен бюджет сегмента");
        void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        // Увеличиваем used только после успешной аллокации. Нехватка памяти в
        // системном allocator не должна оставить счётчик с несуществующими байтами.
        used += bytes;
        peak = std::max(peak, used);
        return p;
    }
    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        used -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    // Одинаковые лимиты не делают два ресурса взаимозаменяемыми: каждый ведёт
    // свой used, поэтому освобождать память нужно через тот же экземпляр.
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    uint64_t limit_;
};
struct TermHash {
    // Прозрачный поиск принимает string_view без временной pmr::string.
    // Иначе оценка памяти сама аллоцировала бы ключ для каждого проверяемого терма.
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};
using TermMap =
    std::pmr::unordered_map<std::pmr::string, std::pmr::vector<Posting>, TermHash, std::equal_to<>>;

// Три выходных потока растут синхронно: словарная запись описывает следующий
// диапазон postings, а offsets позволяет позднее найти запись произвольной длины.
// Вызывающий код обязан подавать термы по возрастанию и ровно df записей на терм.
class SegmentWriter {
  public:
    explicit SegmentWriter(const fs::path& directory) {
        fs::create_directory(directory);
        terms_.open(directory / "terms.data", std::ios::binary);
        offsets_.open(directory / "terms.offsets", std::ios::binary);
        postings_.open(directory / "postings.data", std::ios::binary);
        check_stream(terms_, "Создание словаря сегмента");
        check_stream(offsets_, "Создание смещений сегмента");
        check_stream(postings_, "Создание postings сегмента");
    }
    void term(std::string_view value, uint64_t df) {
        // tellp на файловом потоке может синхронизировать буфер или вызывать
        // lseek. На миллионах коротких записей это разрушает последовательную
        // буферизованную запись. Длина формата известна, поэтому считаем байты.
        write_u64(offsets_, term_bytes_);
        write_string(terms_, value);
        write_u64(terms_, posting_count * PostingBytes);
        write_u64(terms_, df);
        // 20 = u32 длина + u64 адрес списка + u64 df. Сама строка добавляет size байт.
        term_bytes_ += 20 + value.size();
        ++term_count;
    }
    void posting(Posting posting) {
        write_u32(postings_, posting.doc_id);
        postings_.put(static_cast<char>(posting.fields));
        ++posting_count;
    }
    void copy(std::istream& source, uint64_t count) {
        // Входные списки уже закодированы в нашем формате. При слиянии не нужны
        // распаковка в vector<Posting> и повторная запись каждого ID: копируем
        // ограниченными блоками. Все отправляемые байты проверенного блока прочитаны.
        uint64_t bytes = count * PostingBytes;
        while (bytes) {
            const auto length = std::min<uint64_t>(bytes, copy_buffer_.size());
            source.read(copy_buffer_.data(), static_cast<std::streamsize>(length));
            check_stream(source, "Чтение postings при слиянии");
            postings_.write(copy_buffer_.data(), static_cast<std::streamsize>(length));
            bytes -= length;
        }
        posting_count += count;
    }
    void finish() {
        terms_.flush();
        offsets_.flush();
        postings_.flush();
        check_stream(terms_, "Сохранение словаря сегмента");
        check_stream(offsets_, "Сохранение смещений сегмента");
        check_stream(postings_, "Сохранение postings сегмента");
        terms_.close();
        offsets_.close();
        postings_.close();
    }
    uint64_t term_count = 0, posting_count = 0;

  private:
    std::ofstream terms_, offsets_, postings_;
    uint64_t term_bytes_ = 0;
    // Один буфер на writer, без обнуления 128 КиБ для каждого короткого списка.
    // Перед записью весь используемый диапазон заполняется успешным read.
    std::array<char, 128 * 1024> copy_buffer_;
};
struct SegmentReader {
    explicit SegmentReader(const fs::path& path)
        : terms(path / "terms.data", std::ios::binary), postings(path / "postings.data", std::ios::binary) {
        check_stream(terms, "Открытие словаря сегмента");
        check_stream(postings, "Открытие postings сегмента");
        next();
    }
    void next() {
        // EOF допустим только перед новой записью. read_string/read_u64 внутри
        // записи считают преждевременный конец повреждением, а не пустым сегментом.
        present = terms.peek() != std::char_traits<char>::eof();
        if (present) {
            term = read_string(terms);
            info.offset = read_u64(terms);
            info.df = read_u64(terms);
        } else if (terms.bad())
            throw std::runtime_error("Ошибка чтения словаря сегмента");
    }
    std::ifstream terms, postings;
    std::string term;
    TermInfo info;
    uint64_t posting_offset = 0;
    bool present = false;
};

void merge_segments(const std::vector<fs::path>& sources, const fs::path& destination) {
    // sources должны следовать диапазонам doc_id, не произвольному порядку имён.
    // Открываются лишь два файла на вход группы; offsets для последовательного
    // чтения словаря не нужен. fan-in ограничен вызывающим IndexBuilder.
    std::vector<std::unique_ptr<SegmentReader>> readers;
    for (const auto& path : sources)
        readers.push_back(std::make_unique<SegmentReader>(path));
    auto greater = [&](size_t a, size_t b) {
        return readers[a]->term > readers[b]->term;
    };
    // В priority_queue по умолчанию максимум; обратное сравнение даёт минимум.
    // Куча хранит номера читателей, чтобы не копировать строки при перестановках.
    std::priority_queue<size_t, std::vector<size_t>, decltype(greater)> heap(greater);
    for (size_t i = 0; i < readers.size(); ++i)
        if (readers[i]->present)
            heap.push(i);
    SegmentWriter writer(destination);
    // В куче только один текущий терм каждого сегмента: память O(fan_in).
    // Сегменты содержат непересекающиеся последовательные диапазоны doc_id.
    // Поэтому списки одного терма достаточно конкатенировать в порядке сегментов:
    // каждый следующий doc_id уже больше последнего записанного. Эта инварианта
    // сохраняется на всех проходах слияния; глобальная перенумерация не нужна.
    while (!heap.empty()) {
        // Собираем все входы с минимальным текущим термом. Только после этого
        // известен итоговый df и можно записать одну словарную запись результата.
        const std::string term = readers[heap.top()]->term;
        std::vector<size_t> matches;
        uint64_t df = 0;
        while (!heap.empty() && readers[heap.top()]->term == term) {
            size_t index = heap.top();
            heap.pop();
            matches.push_back(index);
            df += readers[index]->info.df;
        }
        // Равные термы выходят из кучи в неопределённом порядке. Сортировка
        // номеров восстанавливает порядок диапазонов doc_id перед конкатенацией.
        std::sort(matches.begin(), matches.end());
        writer.term(term, df);
        for (size_t index : matches) {
            auto& reader = *readers[index];
            // Словарь и postings каждого входа записаны в одинаковом порядке.
            // Этот вход ещё не читался дальше своего текущего терма: позиция уже
            // правильная. seekg здесь сбрасывал бы буфер на каждом коротком списке,
            // превращая потоковое слияние в миллионы мелких read/lseek.
            if (reader.info.offset != reader.posting_offset)
                throw std::runtime_error("Непоследовательный сегмент");
            writer.copy(reader.postings, reader.info.df);
            reader.posting_offset += reader.info.df * PostingBytes;
            // Только снятый из кучи читатель меняет текущий ключ. Обновлять
            // ключ элемента внутри кучи нельзя: нарушился бы её порядок.
            reader.next();
            if (reader.present)
                heap.push(index);
        }
    }
    writer.finish();
}

struct AnalyzedDocument {
    // document после анализа содержит метаданные и выбранный префикс body;
    // terms — собственные строки. Заимствований из буфера чтения здесь нет.
    Document document;
    std::vector<std::pair<std::string, uint8_t>> terms;
};
// Постоянные рабочие потоки исключают создание нового потока для каждого из
// миллионов документов. packaged_task переносит в future и результат, и исключение.
class AnalyzerPool {
  public:
    explicit AnalyzerPool(uint32_t count) {
        try {
            for (uint32_t i = 0; i < count; ++i) {
                workers_.emplace_back([this] {
                    while (true) {
                        std::packaged_task<AnalyzedDocument()> task;
                        {
                            std::unique_lock lock(mutex_);
                            condition_.wait(lock, [&] {
                                return stopping_ || !tasks_.empty();
                            });
                            if (tasks_.empty())
                                return;
                            task = std::move(tasks_.front());
                            tasks_.pop_front();
                        }
                        // Дорогой анализ выполняется вне mutex, иначе все рабочие
                        // потоки фактически последовательно держали бы общий замок.
                        task();
                    }
                });
            }
        } catch (...) {
            // Если ОС отказала в создании очередного потока, уже созданные
            // потоки нужно присоединить даже при незавершённом конструкторе.
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            condition_.notify_all();
            for (auto& worker : workers_)
                worker.join();
            throw;
        }
    }
    ~AnalyzerPool() {
        // stopping запрещает ожидание новых задач, но уже принятые задачи
        // дочитываются: worker выходит лишь при пустой очереди после пробуждения.
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_)
            worker.join();
    }
    std::future<AnalyzedDocument> submit(Document document, uint32_t snippet_bytes) {
        std::packaged_task<AnalyzedDocument()> task(
            [document = std::move(document), snippet_bytes]() mutable {
                auto terms = TextAnalyzer::analyze(document);
                // Сначала индексируем весь текст, затем убираем ненужную выдаче часть.
                // Иначе выключенные сниппеты случайно выключили бы индексирование body.
                document.headings.clear();
                if (document.body.size() > snippet_bytes) {
                    size_t length = snippet_bytes;
                    while (length > 0 && (static_cast<unsigned char>(document.body[length]) & 0xc0) == 0x80)
                        --length;
                    document.body.resize(length);
                }
                return AnalyzedDocument{std::move(document), std::move(terms)};
            });
        auto future = task.get_future();
        // Получатель может ждать future независимо от времени завершения задачи.
        // Захваченный по значению Document принадлежит задаче, не потоку чтения.
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
        return future;
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    std::deque<std::packaged_task<AnalyzedDocument()>> tasks_;
    std::vector<std::thread> workers_;
};

// Чтение gzip/JSON и наполнение хеш-таблицы нагружают процессор независимо.
// Ограниченный конвейер позволяет выполнять их одновременно, не накапливая
// корпус в RAM. Очередь содержит futures в исходном порядке, поэтому даже
// завершившийся раньше соседний документ не получает чужой doc_id.
class DocumentPipeline {
  public:
    explicit DocumentPipeline(const BuildOptions& options)
        : pool_(options.threads), max_pending_(size_t(options.threads) * 8),
          max_bytes_(std::min<uint64_t>(8 * 1024 * 1024, options.memory_limit_mb * 1024 * 1024 / 8)) {
        reader_ = std::thread([this, options] {
            try {
                uint64_t submitted = 0;
                CorpusReader::read(options.corpus, [&](Document&& document) {
                    const uint64_t bytes = document.docid.size() + document.url.size() +
                                           document.title.size() + document.headings.size() +
                                           document.body.size();
                    std::unique_lock lock(mutex_);
                    // Если основной поток не успевает добавлять документы в индекс,
                    // поток чтения ждёт места. Очередь ограничена числом задач
                    // и суммарным размером исходного текста.
                    // Один документ крупнее порога разрешён при пустой очереди,
                    // иначе он никогда не смог бы пройти и возник бы deadlock.
                    condition_.wait(lock, [&] {
                        return cancelled_ || (pending_.size() < max_pending_ &&
                                              (pending_.empty() ||
                                               bytes <= max_bytes_ - std::min(max_bytes_, queued_bytes_)));
                    });
                    if (cancelled_)
                        return false;
                    pending_.push_back({pool_.submit(std::move(document), options.snippet_bytes), bytes});
                    queued_bytes_ += bytes;
                    ++submitted;
                    // Снимаем mutex перед уведомлением: основной поток сразу
                    // заберёт следующую задачу, не ожидая освобождения замка.
                    lock.unlock();
                    condition_.notify_all();
                    return !options.max_docs || submitted < options.max_docs;
                });
            } catch (...) {
                // Исключение не может перескочить границу std::thread: без catch
                // оно завершило бы процесс. Передаём его основному потоку через
                // exception_ptr; он выполнит общее удаление рабочего каталога.
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
            }
            {
                std::lock_guard lock(mutex_);
                done_ = true;
            }
            condition_.notify_all();
        });
    }
    ~DocumentPipeline() {
        // При ошибке основной поток может перестать забирать документы.
        // Будим поток чтения перед join, чтобы завершение не зависло.
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        condition_.notify_all();
        reader_.join();
    }
    std::optional<AnalyzedDocument> next() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] {
            return done_ || !pending_.empty();
        });
        if (pending_.empty()) {
            // Сначала основной поток забирает все принятые документы.
            // Затем next() возвращает конец корпуса или ошибку чтения.
            if (failure_)
                std::rethrow_exception(failure_);
            return std::nullopt;
        }
        auto pending = std::move(pending_.front());
        pending_.pop_front();
        queued_bytes_ -= pending.bytes;
        lock.unlock();
        condition_.notify_all();
        // get ждёт только выбранный в порядке входа документ и переносит ошибку
        // анализатора в основной поток. Mutex очереди во время ожидания не удерживаем.
        return pending.result.get();
    }

  private:
    struct Pending {
        std::future<AnalyzedDocument> result;
        uint64_t bytes;
    };
    // Порядок членов важен для времени жизни: reader присоединяется в теле
    // деструктора, пока очередь/mutex/pool ещё существуют. Затем pool уничтожается
    // последним и присоединяет собственных рабочих до освобождения их состояния.
    AnalyzerPool pool_;
    size_t max_pending_;
    uint64_t max_bytes_, queued_bytes_ = 0;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Pending> pending_;
    bool cancelled_ = false, done_ = false;
    std::exception_ptr failure_;
    std::thread reader_;
};
}

std::unique_ptr<PostingIterator> vector_iterator(std::vector<uint32_t> docs, AdvanceMode mode,
                                                 IteratorMetrics* metrics) {
    return std::make_unique<VectorIterator>(std::move(docs), mode, metrics);
}
TermDictionary::TermDictionary(const fs::path& directory)
    : offsets_(directory / "terms.offsets"), data_(directory / "terms.data") {
    if (offsets_.size() % 8)
        throw std::runtime_error("Повреждены смещения словаря");
}
std::pair<std::string_view, TermInfo> TermDictionary::at(uint64_t ordinal) const {
    if (ordinal >= size())
        throw std::out_of_range("Несуществующий терм");
    // ordinal * 8 адресует фиксированный массив; полученный offset уже адресует
    // строку переменной длины. Чтение через at проверяет каждый диапазон до доступа.
    uint64_t offset = load_u64(offsets_.at(ordinal * 8, 8));
    uint32_t length = load_u32(data_.at(offset, 4));
    offset += 4;
    std::string_view term(data_.at(offset, length), length);
    offset += length;
    const char* info = data_.at(offset, 16);
    return {term, {load_u64(info), load_u64(info + 8)}};
}
std::optional<TermInfo> TermDictionary::find(std::string_view term) const {
    // Ищем первый словарный ключ >= term. Словарь не копируется в RAM; каждый
    // шаг читает только одну запись через таблицу смещений: O(log V) сравнений
    // строк, где стоимость самого сравнения зависит от длины общего префикса.
    uint64_t low = 0, high = size();
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        const auto entry = at(middle);
        if (entry.first < term)
            low = middle + 1;
        else
            high = middle;
    }
    // lower_bound может указывать на соседнее слово или на конец. Совпадение
    // нужно проверить отдельно, чтобы отсутствующее слово не получило чужой список.
    if (low < size()) {
        auto entry = at(low);
        if (entry.first == term)
            return entry.second;
    }
    return std::nullopt;
}
DiskIndex::DiskIndex(const fs::path& directory)
    : dictionary_(directory), postings_(std::make_shared<MappedFile>(directory / "postings.data")),
      documents_(directory) {
    std::ifstream in(directory / "manifest.json");
    check_stream(in, "Открытие манифеста");
    const auto manifest = nlohmann::json::parse(in);
    // Те же байты при другой версии формата/нормализации могли бы означать
    // другое содержимое. Отказываем явно, вместо выдачи правдоподобных ошибок поиска.
    if (manifest.at("format") != "boolean-search-1" || manifest.at("analyzer") != "icu-simple-lower-1")
        throw std::runtime_error("Несовместимый формат индекса или анализатор");
    // Это дешёвые проверки файлов целиком по размерам. Сортировку миллиардов
    // записей проверяет validate; запуск обычного запроса не должен ждать такого прохода.
    if (postings_->size() % PostingBytes || manifest.at("documents") != documents_.size() ||
        manifest.at("terms") != dictionary_.size() || manifest.at("postings") != posting_count())
        throw std::runtime_error("Размеры файлов индекса не соответствуют манифесту");
}
std::unique_ptr<PostingIterator> DiskIndex::postings(std::string_view term, uint8_t field, AdvanceMode mode,
                                                     IteratorMetrics* metrics) const {
    auto info = dictionary_.find(term);
    // Пустой поток участвует в тех же булевых операциях, что непустой. Специальные
    // ветки обработки «слова нет в словаре» не распространяются по дереву запроса.
    return std::make_unique<DiskPostingIterator>(postings_, info.value_or(TermInfo{}), field, mode, metrics);
}

BuildStats IndexBuilder::build(const BuildOptions& options) {
    // Проверяем параметры до создания любых выходных файлов. Конечный каталог
    // занятого индекса не используется как рабочий даже для первого сегмента.
    if (options.memory_limit_mb < 8 || options.memory_limit_mb > (uint64_t{1} << 30) ||
        options.threads == 0 || options.threads > 256 || options.merge_fan_in < 2 ||
        options.merge_fan_in > 64)
        throw std::invalid_argument("Память >= 8 МиБ, threads 1..256, merge-fan-in 2..64");
    if (options.output.empty() || fs::exists(options.output))
        throw std::runtime_error("Выходной каталог уже существует или не задан");
    fs::create_directories(options.output.parent_path().empty() ? fs::path(".")
                                                                : options.output.parent_path());
    const fs::path work = options.output.string() + ".building-" + std::to_string(getpid());
    if (!fs::create_directory(work))
        throw std::runtime_error("Временный каталог уже существует: " + work.string());
    const auto started = Clock::now();
    BuildStats stats;
    try {
        DocumentStoreWriter documents(work);
        // Основной индекс растёт только до этой квоты, независимо от размера
        // корпуса. Резервная четверть нужна массиву указателей для сортировки,
        // очереди анализа и другим буферам; жёсткий предел RSS этим не задаётся.
        BudgetResource resource(options.memory_limit_mb * 1024 * 1024 * 3 / 4);
        TermMap terms(&resource);
        std::vector<fs::path> segments;
        uint64_t segment_documents = 0;
        std::vector<std::pmr::vector<Posting>*> posting_targets;
        auto flush = [&] {
            // Пустой по документам сегмент не нужен. Сегмент с пустыми термами,
            // но реальными пустыми документами, допустим: их ID уже заняты в store.
            if (!segment_documents)
                return;
            fs::path segment = work / ("segment-" + std::to_string(segments.size()));
            SegmentWriter writer(segment);
            // Хеш-таблица даёт ожидаемое O(1) на терм во время индексации. Лишь
            // перед сбросом сортируем указатели, а не копии строк и postings.
            // Массив указателей занимает 8 байт на терм из резервной четверти RAM.
            std::vector<const TermMap::value_type*> sorted;
            sorted.reserve(terms.size());
            for (const auto& entry : terms)
                sorted.push_back(&entry);
            std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
                return a->first < b->first;
            });
            for (const auto* entry : sorted) {
                const auto& [term, postings] = *entry;
                writer.term(term, postings.size());
                for (const auto& posting : postings)
                    writer.posting(posting);
            }
            writer.finish();
            segments.push_back(segment);
            // clear удалил бы элементы, но сохранил большой массив buckets.
            // Swap с пустой таблицей и её уничтожение освобождают и этот массив.
            {
                TermMap empty(&resource);
                terms.swap(empty);
            }
            segment_documents = 0;
            ++stats.segments;
        };
        auto required = [&](const AnalyzedDocument& document) {
            // Оцениваем ДО изменения таблицы, чтобы не разрезать один документ
            // между сегментами. Заодно запоминаем адреса уже существующих списков:
            // следующий проход вставки не должен повторять все хеш-поиски.
            uint64_t bytes = 0, new_terms = 0;
            posting_targets.clear();
            posting_targets.reserve(document.terms.size());
            for (const auto& [term, field] : document.terms) {
                (void)field;
                const auto it = terms.find(std::string_view(term));
                posting_targets.push_back(it == terms.end() ? nullptr : &it->second);
                // Новый терм требует узел, строку и первый posting. Запас 256 байт
                // консервативно покрывает служебную часть узла используемой STL.
                if (it == terms.end()) {
                    bytes += 256 + term.size() + 1 + sizeof(Posting);
                    ++new_terms;
                }
                // При росте вектора новый буфер временно существует вместе со
                // старым; нужные байты считаем до освобождения прежней capacity.
                else if (it->second.size() == it->second.capacity())
                    bytes += std::max<size_t>(1, it->second.capacity() * 2) * sizeof(Posting);
            }
            // При удвоении reserve учитываем и старые, и новые buckets, которые
            // кратковременно живут одновременно. Резерв в 4 указателя на элемент
            // покрывает округление количества buckets стандартной библиотекой.
            const uint64_t size = terms.size() + new_terms;
            if (size > terms.bucket_count())
                bytes += size * 4 * sizeof(void*);
            return std::pair{bytes, size};
        };
        DocumentPipeline pipeline(options);
        while (auto next = pipeline.next()) {
            auto& analyzed = *next;
            auto [needed, next_size] = required(analyzed);
            // После flush все сохранённые адреса posting_targets устарели,
            // поэтому оценку и адреса обязательно вычисляем повторно.
            if (resource.used + needed > resource.limit()) {
                flush();
                std::tie(needed, next_size) = required(analyzed);
            }
            if (resource.used + needed > resource.limit())
                throw std::runtime_error(
                    "Термы одного документа превышают бюджет сегмента; увеличьте --memory-limit-mb");
            if (next_size > terms.bucket_count())
                terms.reserve(next_size * 2);
            if (stats.documents >= UINT32_MAX)
                throw std::runtime_error("Превышен диапазон внутренних uint32 doc_id");
            // Назначаем ID здесь, в одном потоке, после ожидания нужного future.
            // Порядок завершения анализаторов не влияет ни на ID, ни на сортировку.
            auto id = static_cast<uint32_t>(stats.documents);
            for (size_t i = 0; i < analyzed.terms.size(); ++i) {
                auto& [term, fields] = analyzed.terms[i];
                // Контейнеры с поддержкой аллокатора передают ресурс вложенным
                // строкам и векторам: ключи и posting lists учитываются счётчиком.
                // rehash не инвалидирует указатели на элементы unordered_map.
                // Используем найденный при оценке вектор повторно и экономим
                // второй поиск для термов, уже встречавшихся в сегменте.
                auto* target = posting_targets[i];
                if (!target)
                    target = &terms.try_emplace(std::pmr::string(term, &resource)).first->second;
                // analyze уже удалил повторы терма внутри документа, а следующий
                // документ имеет больший ID. push_back сразу сохраняет строгий
                // порядок списка; поздняя сортировка самих postings не требуется.
                target->push_back({id, fields});
            }
            documents.append(analyzed.document, options.snippet_bytes);
            ++stats.documents;
            ++segment_documents;
            if (options.segment_docs && segment_documents >= options.segment_docs)
                flush();
            if (stats.documents % 100000 == 0)
                std::cerr << "Проиндексировано " << stats.documents << ", сегментов " << stats.segments
                          << '\n';
        }
        flush();
        documents.finish();
        stats.max_tracked_bytes = resource.peak;
        // Единый пустой сегмент позволяет опубликовать корректный индекс нулевого
        // корпуса через тот же путь, без отсутствующих обязательных файлов.
        if (segments.empty()) {
            auto segment = work / "segment-empty";
            SegmentWriter writer(segment);
            writer.finish();
            segments.push_back(segment);
        }
        uint64_t pass = 0;
        while (segments.size() > 1) {
            // Один проход заменяет соседние группы максимум fan-in входов.
            // Вектор next сохраняет хронологический порядок диапазонов doc_id;
            // нельзя переставлять пути по лексикографическим именам segment-10/segment-2.
            std::vector<fs::path> next;
            for (size_t first = 0; first < segments.size(); first += options.merge_fan_in) {
                const auto last = std::min(segments.size(), first + options.merge_fan_in);
                std::vector<fs::path> group(segments.begin() + static_cast<std::ptrdiff_t>(first),
                                            segments.begin() + static_cast<std::ptrdiff_t>(last));
                // Остаточный одиночный сегмент уже отсортирован: переносим ссылку
                // на него в следующий проход без бесполезного копирования файла.
                if (group.size() == 1) {
                    next.push_back(group.front());
                    continue;
                }
                auto destination = work / ("merge-" + std::to_string(pass) + "-" + std::to_string(first));
                merge_segments(group, destination);
                next.push_back(destination);
                // Исходные файлы группы удаляются после успешного finish выхода,
                // когда читатели закрыты. Остальные группы пока остаются на диске.
                for (const auto& old : group)
                    fs::remove_all(old);
            }
            segments = std::move(next);
            ++pass;
        }
        for (const auto* name : {"terms.data", "terms.offsets", "postings.data"})
            fs::rename(segments.front() / name, work / name);
        fs::remove(segments.front());
        stats.terms = fs::file_size(work / "terms.offsets") / 8;
        stats.postings = fs::file_size(work / "postings.data") / PostingBytes;
        stats.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        stats.peak_rss_kb = peak_rss_kb();
        // Манифест описывает именно завершённые файлы, а не ожидаемые оценки:
        // число термов и postings получено из их фактических размеров выше.
        nlohmann::json manifest = {{"format", "boolean-search-1"},
                                   {"analyzer", "icu-simple-lower-1"},
                                   {"documents", stats.documents},
                                   {"terms", stats.terms},
                                   {"postings", stats.postings},
                                   {"segments", stats.segments},
                                   {"build_seconds", stats.seconds},
                                   {"peak_rss_kb", stats.peak_rss_kb},
                                   {"max_tracked_bytes", stats.max_tracked_bytes},
                                   {"threads", options.threads},
                                   {"memory_limit_mb", options.memory_limit_mb},
                                   {"merge_fan_in", options.merge_fan_in},
                                   {"snippet_bytes", options.snippet_bytes},
                                   {"max_docs", options.max_docs},
                                   {"corpus", options.corpus.string()}};
        std::ofstream output(work / "manifest.json");
        output << manifest.dump(2) << '\n';
        output.close();
        check_stream(output, "Сохранение манифеста");
        // Манифест появляется последним, а публикация — одним rename. Незавершённое
        // построение никогда не подменяет готовый индекс. При штатной ошибке удаляем
        // только созданный нами временный каталог; исходный корпус не затрагивается.
        stats.index_bytes = directory_size(work);
        fs::rename(work, options.output);
        return stats;
    } catch (...) {
        fs::remove_all(work);
        throw;
    }
}

void validate_index(const fs::path& directory) {
    // Полный аудит намеренно дорогой: последовательно читаются V термов,
    // P postings и все записи метаданных. Он отделён от быстрого открытия индекса.
    DiskIndex index(directory);
    MappedFile raw(directory / "postings.data");
    uint64_t total = 0;
    std::string previous;
    for (uint64_t i = 0; i < index.dictionary().size(); ++i) {
        const auto [term, info] = index.dictionary().at(i);
        // Следующий список обязан начинаться сразу после предыдущего. Это
        // выявляет разрывы, перекрытия и ложные df, даже если адрес ещё внутри файла.
        if (term.empty() || (i && previous >= term) || info.offset != total * PostingBytes || !info.df)
            throw std::runtime_error("Нарушен порядок словаря или границы postings");
        previous = term;
        uint64_t count = 0;
        uint32_t last = 0;
        if (info.df > raw.size() / PostingBytes)
            throw std::runtime_error("Повреждена df");
        const char* data = raw.at(info.offset, info.df * PostingBytes);
        // Проверка идёт последовательно по файлу, без O(log V) повторного поиска
        // каждого уже прочитанного терма в словаре.
        while (count < info.df) {
            const auto id = load_u32(data + count * PostingBytes);
            const auto mask = static_cast<uint8_t>(data[count * PostingBytes + 4]);
            if (!mask || mask > 7)
                throw std::runtime_error("Повреждена маска полей");
            // Одновременно проверяем принадлежность doc_id корпусу и отсутствие
            // повторов/убывания. Дубликат также нарушает строгое id > last.
            if (id >= index.documents().size() || (count && id <= last))
                throw std::runtime_error("Нарушен порядок doc_id");
            last = id;
            ++count;
        }
        if (count != info.df)
            throw std::runtime_error("df не совпадает с длиной списка");
        total += count;
    }
    if (total != index.posting_count())
        throw std::runtime_error("Лишние postings в файле");
    for (uint64_t i = 0; i < index.documents().size(); ++i)
        index.documents().get(static_cast<uint32_t>(i));
}
}
