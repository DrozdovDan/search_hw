#include "search/web.hpp"
#include "search/corpus.hpp"
#include "search/query.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

namespace search {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopping = 0;
void stop_server(int) {
    stopping = 1;
}

struct HttpError : std::runtime_error {
    int status;
    HttpError(int code, const std::string& message) : std::runtime_error(message), status(code) {}
};
struct Socket {
    int fd;
    explicit Socket(int value) : fd(value) {}
    ~Socket() {
        if (fd >= 0)
            ::close(fd);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

uint64_t number(std::string_view value, uint64_t maximum = UINT64_MAX) {
    uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || result > maximum)
        throw HttpError(400, "Некорректное числовое значение");
    return result;
}
uint64_t json_number(const Json& object, const char* key, uint64_t fallback, uint64_t maximum) {
    const auto it = object.find(key);
    if (it == object.end())
        return fallback;
    if (!it->is_number_unsigned() && !(it->is_number_integer() && it->get<int64_t>() >= 0))
        throw HttpError(400, std::string("Ожидается неотрицательное целое: ") + key);
    const auto value = it->get<uint64_t>();
    if (value > maximum)
        throw HttpError(400, std::string("Слишком большое значение: ") + key);
    return value;
}
std::string decode(std::string_view value) {
    std::string result;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+')
            result += ' ';
        else if (value[i] == '%') {
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                throw HttpError(400, "Некорректное кодирование URL");
            const char c = static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            if (!c)
                throw HttpError(400, "Нулевой байт в URL");
            result += c;
            i += 2;
        } else
            result += value[i];
    }
    return result;
}
struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> parameters;
};
Request receive_request(int fd) {
    std::string input;
    std::array<char, 8192> buffer{};
    size_t header_end = std::string::npos;
    while ((header_end = input.find("\r\n\r\n")) == std::string::npos) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0)
            throw HttpError(400, "Не удалось прочитать HTTP-запрос");
        input.append(buffer.data(), static_cast<size_t>(count));
        if (input.size() > 24 * 1024)
            throw HttpError(413, "Слишком большой HTTP-заголовок");
    }
    std::istringstream headers(input.substr(0, header_end));
    std::string line, target, version;
    std::getline(headers, line);
    Request request;
    std::istringstream first(line);
    first >> request.method >> target >> version;
    if ((version != "HTTP/1.1" && version != "HTTP/1.0") || target.empty() || target[0] != '/')
        throw HttpError(400, "Некорректная строка HTTP-запроса");
    uint64_t content_length = 0;
    bool has_length = false;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            throw HttpError(400, "Некорректный HTTP-заголовок");
        auto key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        auto value = line.substr(colon + 1);
        const auto start = value.find_first_not_of(" \t");
        value = start == std::string::npos ? "" : value.substr(start);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        if (key == "transfer-encoding")
            throw HttpError(400, "Transfer-Encoding не поддерживается");
        if (key == "content-length") {
            if (has_length)
                throw HttpError(400, "Повтор Content-Length");
            has_length = true;
            content_length = number(value, 64 * 1024);
        }
    }
    request.body = input.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const auto count = ::recv(fd, buffer.data(),
                                  std::min<uint64_t>(buffer.size(), content_length - request.body.size()), 0);
        if (count <= 0)
            throw HttpError(400, "Неполное тело HTTP-запроса");
        request.body.append(buffer.data(), static_cast<size_t>(count));
    }
    request.body.resize(static_cast<size_t>(content_length));
    const auto question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string::npos) {
        std::string_view rest(target.data() + question + 1, target.size() - question - 1);
        while (!rest.empty()) {
            const auto amp = rest.find('&');
            const auto pair = rest.substr(0, amp);
            const auto equals = pair.find('=');
            const auto key = decode(pair.substr(0, equals));
            const auto value = equals == std::string::npos ? "" : decode(pair.substr(equals + 1));
            if (!request.parameters.emplace(key, value).second)
                throw HttpError(400, "Повтор параметра URL");
            if (amp == std::string::npos)
                break;
            rest.remove_prefix(amp + 1);
        }
    }
    return request;
}
void send_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return;
        bytes.remove_prefix(static_cast<size_t>(sent));
    }
}
void respond(int fd, int status, const std::string& body,
             const std::string& type = "application/json; charset=utf-8") {
    const std::string reason = status == 200   ? "OK"
                               : status == 404 ? "Not Found"
                               : status == 405 ? "Method Not Allowed"
                               : status == 413 ? "Payload Too Large"
                               : status == 503 ? "Service Unavailable"
                               : status == 500 ? "Internal Server Error"
                                               : "Bad Request";
    std::ostringstream header;
    header
        << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Type: " << type
        << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\nCache-Control: no-store"
        << "\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer"
        << "\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; "
           "img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n";
    send_all(fd, header.str());
    send_all(fd, body);
}

// MS MARCO кодирует номер gzip-шарда и смещение JSON-строки в docid. Смещение
// относится к РАСПАКОВАННОМУ потоку. Проверяем docid прочитанного JSON: ошибочное
// предположение о формате никогда не должно показывать соседний документ.
struct CorpusPosition {
    std::string file;
    uint64_t offset;
};
std::optional<CorpusPosition> corpus_position(std::string_view docid) {
    constexpr std::string_view prefix = "msmarco_doc_";
    if (!docid.starts_with(prefix))
        return std::nullopt;
    const auto rest = docid.substr(prefix.size());
    const auto separator = rest.find('_');
    if (separator == std::string_view::npos || separator == 0)
        return std::nullopt;
    for (char c : rest.substr(0, separator))
        if (c < '0' || c > '9')
            return std::nullopt;
    try {
        return CorpusPosition{"msmarco_doc_" + std::string(rest.substr(0, separator)) + ".gz",
                              number(rest.substr(separator + 1), uint64_t(INT64_MAX))};
    } catch (const HttpError&) {
        return std::nullopt;
    }
}
struct ArchiveEntry {
    uint64_t offset, bytes;
};
uint64_t tar_number(const char* value, size_t length) {
    uint64_t result = 0;
    for (size_t i = 0; i < length; ++i) {
        if (!value[i] || value[i] == ' ')
            continue;
        if (value[i] < '0' || value[i] > '7' || result > (UINT64_MAX >> 3))
            throw std::runtime_error("Повреждён заголовок tar");
        result = result * 8 + static_cast<unsigned>(value[i] - '0');
    }
    return result;
}
std::map<std::string, ArchiveEntry> archive_entries(const fs::path& source) {
    std::ifstream stream(source, std::ios::binary);
    check_stream(stream, "Открытие архива корпуса");
    const auto file_size = fs::file_size(source);
    uint64_t position = 0;
    std::map<std::string, ArchiveEntry> entries;
    std::array<char, 512> header{};
    while (position <= file_size && file_size - position >= header.size()) {
        stream.seekg(static_cast<std::streamoff>(position));
        stream.read(header.data(), header.size());
        check_stream(stream, "Чтение tar");
        if (std::all_of(header.begin(), header.end(), [](char c) {
                return c == 0;
            }))
            return entries;
        uint64_t checksum = 0;
        for (size_t i = 0; i < header.size(); ++i)
            checksum += i >= 148 && i < 156 ? 32 : static_cast<unsigned char>(header[i]);
        if (checksum != tar_number(header.data() + 148, 8))
            throw std::runtime_error("Неверная checksum tar");
        const auto length = tar_number(header.data() + 124, 12);
        const auto start = position + header.size();
        if (length > file_size - start || length > UINT64_MAX - 511)
            throw std::runtime_error("Обрезанный tar");
        std::string name(header.data(), strnlen(header.data(), 100));
        if (header[156] == '0' || header[156] == '\0')
            entries.emplace(fs::path(name).filename().string(), ArchiveEntry{start, length});
        const auto padded = (length + 511) / 512 * 512;
        if (padded > file_size - start)
            throw std::runtime_error("Обрезанный padding tar");
        position = start + padded;
    }
    throw std::runtime_error("Нет завершающего блока tar");
}
std::string gzip_line(const fs::path& file, ArchiveEntry entry, uint64_t target) {
    std::ifstream input(file, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(entry.offset));
    check_stream(input, "Открытие gzip-шарда");
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK)
        throw std::runtime_error("Не удалось открыть gzip");
    struct Guard {
        z_stream* stream;
        ~Guard() { inflateEnd(stream); }
    } guard{&stream};
    std::array<unsigned char, 128 * 1024> compressed{}, output{};
    uint64_t remaining = entry.bytes, decoded = 0;
    std::string line;
    while (true) {
        if (!stream.avail_in && remaining) {
            const auto count = std::min<uint64_t>(remaining, compressed.size());
            input.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(count));
            check_stream(input, "Чтение gzip-шарда");
            remaining -= count;
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());
        const auto before = stream.avail_in;
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            throw std::runtime_error("Ошибка распаковки исходного документа");
        const auto produced = output.size() - stream.avail_out;
        if (target < decoded + produced) {
            const size_t start = target > decoded ? static_cast<size_t>(target - decoded) : 0;
            const auto* begin = reinterpret_cast<const char*>(output.data()) + start;
            const auto* end = static_cast<const char*>(std::memchr(begin, '\n', produced - start));
            const auto count = end ? static_cast<size_t>(end - begin) : produced - start;
            if (line.size() + count > 64 * 1024 * 1024)
                throw std::runtime_error("Документ превышает 64 МиБ");
            line.append(begin, count);
            if (end)
                return line;
        }
        decoded += produced;
        if (status == Z_STREAM_END) {
            if (!remaining && !stream.avail_in)
                break;
            if (inflateReset2(&stream, 15 + 16) != Z_OK)
                throw std::runtime_error("Ошибка следующего gzip member");
        } else if (!produced && stream.avail_in == before && !remaining)
            throw std::runtime_error("Обрезанный gzip корпуса");
    }
    if (line.empty())
        throw std::runtime_error("Смещение документа за концом gzip");
    return line;
}
std::string fingerprint(const std::string& value) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
Json document_json(const Document& document) {
    return {{"docid", document.docid},
            {"url", document.url},
            {"title", document.title},
            {"headings", document.headings},
            {"body", document.body}};
}
class SourceDocuments {
  public:
    SourceDocuments(fs::path source, const fs::path& cache) : source_(fs::weakly_canonical(source)) {
        if (!fs::exists(source_))
            throw HttpError(404, "Исходный корпус не найден на сервере");
        const auto signature =
            source_.string() + ":" + std::to_string(fs::last_write_time(source_).time_since_epoch().count()) +
            (fs::is_regular_file(source_) ? ":" + std::to_string(fs::file_size(source_)) : "");
        cache_ = cache / fingerprint(signature);
        fs::create_directories(cache_);
        if (source_.extension() == ".tar")
            entries_ = archive_entries(source_);
    }
    Json get(const std::string& docid) {
        // Один читатель на источник предотвращает одновременную распаковку одного
        // и того же большого шарда. Готовые ответы сохраняются между перезапусками.
        std::lock_guard lock(mutex_);
        const auto cached = cache_ / (fingerprint(docid) + ".json");
        if (fs::exists(cached)) {
            try {
                std::ifstream input(cached);
                auto value = Json::parse(input);
                if (value.at("docid") == docid && value.at("body").is_string() &&
                    value.at("title").is_string() && value.at("headings").is_string())
                    return value;
            } catch (const std::exception&) { /* Повреждённый кэш восстанавливается из корпуса. */
            }
        }
        Json value;
        if (const auto position = corpus_position(docid); position && !entries_.empty()) {
            const auto entry = entries_.find(position->file);
            if (entry == entries_.end())
                throw HttpError(404, "Шард документа отсутствует в корпусе");
            value = Json::parse(gzip_line(source_, entry->second, position->offset));
            if (value.value("docid", "") != docid)
                throw std::runtime_error("Смещение docid не соответствует корпусу");
            // Приводим null/отсутствующие текстовые поля к тому же представлению,
            // которое использует CorpusReader во время индексирования.
            for (const auto* key : {"title", "headings", "body", "url"}) {
                if (!value.contains(key) || value[key].is_null())
                    value[key] = "";
                if (!value[key].is_string())
                    throw std::runtime_error("Некорректное поле исходного документа");
            }
        } else {
            // Универсальный путь для пользовательских JSONL/gzip/каталогов.
            // Официальный tar использует адресацию по docid и не сканируется целиком.
            CorpusReader::read(source_, [&](Document&& document) {
                if (document.docid != docid)
                    return true;
                value = document_json(document);
                return false;
            });
            if (value.is_null())
                throw HttpError(404, "Документ не найден в исходном корпусе");
        }
        const auto temporary = cached.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary);
        output << value.dump();
        output.close();
        check_stream(output, "Сохранение кэша документа");
        fs::rename(temporary, cached);
        return value;
    }

  private:
    fs::path source_, cache_;
    std::map<std::string, ArchiveEntry> entries_;
    std::mutex mutex_;
};

struct IndexEntry {
    fs::path directory;
    Json manifest;
    std::shared_ptr<SearchEngine> engine;
};
class WebApplication {
  public:
    explicit WebApplication(const WebOptions& options) : options_(options) {
        add_index(fs::weakly_canonical(options.index));
        default_ = fs::weakly_canonical(options.index).filename().string();
        if (!indexes_.contains(default_))
            throw std::runtime_error("Не удалось открыть индекс по умолчанию");
        if (!options.indexes_root.empty() && fs::is_directory(options.indexes_root)) {
            for (const auto& entry : fs::directory_iterator(options.indexes_root)) {
                if (!entry.is_directory())
                    continue;
                const auto name = entry.path().filename().string();
                if (name.starts_with("index-") || name.starts_with("my-index-")) {
                    try {
                        add_index(fs::weakly_canonical(entry.path()));
                    } catch (const std::exception&) { /* Незавершённый каталог не предлагается в UI. */
                    }
                }
            }
        }
        (void)engine(default_);
        for (const auto& [name, type] :
             std::map<std::string, std::string>{{"index.html", "text/html; charset=utf-8"},
                                                {"app.css", "text/css; charset=utf-8"},
                                                {"app.js", "text/javascript; charset=utf-8"},
                                                {"text.js", "text/javascript; charset=utf-8"},
                                                {"favicon.svg", "image/svg+xml"}}) {
            std::ifstream input(options.assets / name, std::ios::binary);
            check_stream(input, "Открытие web/" + name);
            assets_.emplace("/" + name,
                            std::pair{std::string(std::istreambuf_iterator<char>(input), {}), type});
        }
    }
    void handle(int fd, const Request& request) {
        if (request.method != "GET" && request.method != "POST")
            throw HttpError(405, "Поддерживаются GET и POST");
        if (request.path == "/api/search") {
            if (request.method != "POST")
                throw HttpError(405, "Поиск ожидает POST");
            respond(fd, 200, search(Json::parse(request.body)).dump());
            return;
        }
        if (request.method != "GET")
            throw HttpError(405, "Ожидается GET");
        if (request.path == "/healthz") {
            respond(fd, 200, Json({{"status", "ok"}}).dump());
            return;
        }
        if (request.path == "/api/indexes") {
            Json items = Json::array();
            for (const auto& [name, entry] : indexes_)
                items.push_back({{"id", name},
                                 {"documents", entry.manifest.at("documents")},
                                 {"terms", entry.manifest.at("terms")}});
            respond(fd, 200, Json({{"default", default_}, {"indexes", items}}).dump());
            return;
        }
        if (request.path == "/api/document") {
            const auto key = parameter(request, "index", default_);
            const auto id = number(parameter(request, "id", ""), UINT32_MAX);
            auto opened = engine(key);
            if (id >= opened->index().documents().size())
                throw HttpError(404, "Документ отсутствует в индексе");
            const auto metadata = opened->index().documents().get(static_cast<uint32_t>(id));
            auto full = source(key)->get(metadata.docid);
            full["internal_id"] = id;
            respond(fd, 200, full.dump());
            return;
        }
        const auto path = request.path == "/" ? "/index.html" : request.path;
        const auto asset = assets_.find(path);
        if (asset == assets_.end())
            throw HttpError(404, "Страница не найдена");
        respond(fd, 200, asset->second.first, asset->second.second);
    }

  private:
    void add_index(const fs::path& directory) {
        std::ifstream input(directory / "manifest.json");
        auto manifest = Json::parse(input);
        if (manifest.at("format") != "boolean-search-1" || manifest.at("analyzer") != "icu-simple-lower-1")
            throw std::runtime_error("Несовместимый индекс");
        const auto name = directory.filename().string();
        indexes_.try_emplace(name, IndexEntry{directory, std::move(manifest), {}});
    }
    std::shared_ptr<SearchEngine> engine(const std::string& name) {
        std::lock_guard lock(mutex_);
        const auto it = indexes_.find(name);
        if (it == indexes_.end())
            throw HttpError(404, "Неизвестный индекс");
        if (!it->second.engine)
            it->second.engine = std::make_shared<SearchEngine>(it->second.directory);
        return it->second.engine;
    }
    std::shared_ptr<SourceDocuments> source(const std::string& name) {
        std::lock_guard lock(mutex_);
        auto path = fs::path(indexes_.at(name).manifest.at("corpus").get<std::string>());
        // Старые манифесты записывали путь относительно каталога запуска CLI.
        // Для обычной структуры проекта проверяем также каталог над data/.
        if (path.is_relative() && !fs::exists(path)) {
            const auto candidate = indexes_.at(name).directory.parent_path().parent_path() / path;
            if (fs::exists(candidate))
                path = candidate;
        }
        const auto key = fs::weakly_canonical(path).string();
        auto& reader = sources_[key];
        if (!reader)
            reader = std::make_shared<SourceDocuments>(path, options_.cache);
        return reader;
    }
    static std::string parameter(const Request& request, const std::string& name,
                                 const std::string& fallback) {
        const auto it = request.parameters.find(name);
        return it == request.parameters.end() ? fallback : it->second;
    }
    Json search(const Json& body) {
        if (!body.is_object())
            throw HttpError(400, "Ожидается JSON-объект");
        const auto query = body.value("query", std::string{});
        if (query.size() > 16 * 1024)
            throw HttpError(400, "Запрос превышает 16 КиБ");
        const auto key = body.value("index", default_);
        const auto limit = json_number(body, "limit", 10, 100);
        if (!limit)
            throw HttpError(400, "Количество результатов должно быть от 1 до 100");
        const auto mode_name = body.value("mode", std::string("binary"));
        if (mode_name != "binary" && mode_name != "sequential")
            throw HttpError(400, "Неизвестный алгоритм");
        const auto mode = mode_name == "binary" ? AdvanceMode::Binary : AdvanceMode::Sequential;
        auto opened = engine(key);
        const auto start = Clock::now();
        std::unique_ptr<QueryNode> ast;
        try {
            ast = QueryParser::parse(query);
        } catch (const std::exception& error) {
            throw HttpError(400, error.what());
        }
        auto cursor = QueryExecutor(opened->index()).execute(*ast, mode);
        if (body.contains("after") && !body["after"].is_null()) {
            const auto after = json_number(body, "after", 0, UINT32_MAX);
            if (after == UINT32_MAX)
                return {{"results", Json::array()},
                        {"has_more", false},
                        {"terms", Json::array()},
                        {"elapsed_ms", 0}};
            cursor->advance(static_cast<uint32_t>(after + 1));
        }
        std::vector<uint32_t> ids;
        while (cursor->valid() && ids.size() <= limit) {
            ids.push_back(cursor->doc_id());
            cursor->next();
        }
        const bool more = ids.size() > limit;
        if (more)
            ids.pop_back();
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::set<std::string> words;
        std::function<void(const QueryNode&)> visit = [&](const QueryNode& node) {
            if (node.kind == QueryKind::Term)
                words.insert(node.term);
            else {
                visit(*node.left);
                visit(*node.right);
            }
        };
        visit(*ast);
        Json results = Json::array();
        for (auto id : ids) {
            const auto document = opened->index().documents().get(id);
            results.push_back({{"internal_id", id},
                               {"docid", document.docid},
                               {"title", document.title},
                               {"url", document.url},
                               {"snippet", document.snippet}});
        }
        return {{"results", results}, {"has_more", more}, {"terms", words}, {"elapsed_ms", elapsed}};
    }
    WebOptions options_;
    std::string default_;
    std::map<std::string, IndexEntry> indexes_;
    std::map<std::string, std::shared_ptr<SourceDocuments>> sources_;
    std::map<std::string, std::pair<std::string, std::string>> assets_;
    std::mutex mutex_;
};

// Ограниченная очередь и постоянные потоки обслуживают несколько браузеров.
// Ошибка запроса превращается в JSON, а не завершает процесс сервера.
class HttpWorkers {
  public:
    HttpWorkers(uint32_t count, WebApplication& application) : application_(application) {
        try {
            for (uint32_t i = 0; i < count; ++i)
                workers_.emplace_back([this] {
                    run();
                });
        } catch (...) {
            finish();
            throw;
        }
    }
    ~HttpWorkers() { finish(); }
    bool submit(int fd) {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= 64)
            return false;
        queue_.push_back(fd);
        condition_.notify_one();
        return true;
    }

  private:
    void finish() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
    }
    void run() {
        while (true) {
            int fd;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return done_ || !queue_.empty();
                });
                if (queue_.empty())
                    return;
                fd = queue_.front();
                queue_.pop_front();
            }
            Socket connection(fd);
            try {
                application_.handle(fd, receive_request(fd));
            } catch (const HttpError& error) {
                respond(fd, error.status, Json({{"error", error.what()}}).dump());
            } catch (const Json::exception& error) {
                respond(fd, 400, Json({{"error", std::string("Некорректный JSON: ") + error.what()}}).dump());
            } catch (const std::exception& error) {
                respond(fd, 500, Json({{"error", error.what()}}).dump());
            }
        }
    }
    WebApplication& application_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<int> queue_;
    std::vector<std::thread> workers_;
    bool done_ = false;
};
}

void serve_web(const WebOptions& options) {
    if (!options.port || !options.workers || options.workers > 32)
        throw std::runtime_error("port: 1..65535, workers: 1..32");
    WebApplication application(options);
    Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.fd < 0)
        throw std::runtime_error("Не удалось создать HTTP-сокет");
    int reuse = 1;
    setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("--host ожидает IPv4-адрес");
    if (::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("Не удалось занять порт: " + std::string(std::strerror(errno)));
    if (::listen(listener.fd, 64) != 0)
        throw std::runtime_error("Не удалось запустить HTTP-сервер");
    stopping = 0;
    const auto previous_int = std::signal(SIGINT, stop_server);
    const auto previous_term = std::signal(SIGTERM, stop_server);
    std::cout << "Search web: http://" << options.host << ':' << options.port << '\n' << std::flush;
    {
        HttpWorkers workers(options.workers, application);
        while (!stopping) {
            fd_set readers;
            FD_ZERO(&readers);
            FD_SET(listener.fd, &readers);
            timeval timeout{1, 0};
            const int ready = select(listener.fd + 1, &readers, nullptr, nullptr, &timeout);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                throw std::runtime_error("Ошибка ожидания HTTP-соединения");
            if (!ready)
                continue;
            const int fd = accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("Ошибка accept");
            }
            timeval receive_timeout{10, 0}, send_timeout{30, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
            if (!workers.submit(fd)) {
                Socket rejected(fd);
                respond(fd, 503, Json({{"error", "Сервер занят, повторите запрос"}}).dump());
            }
        }
    }
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
}
}
