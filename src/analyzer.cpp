#include "search/analyzer.hpp"
#include <algorithm>
#include <stdexcept>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

namespace search {
// Один и тот же путь используется для документов и листьев запроса. Если бы
// запрос приводился к нижнему регистру иначе, существующее слово могло бы не
// найтись в словаре. Функция не меняет вход и владеет всеми строками результата.
std::vector<std::string> TextAnalyzer::tokenize(std::string_view text) {
    // ICU использует int32 для индексов; проверка должна предшествовать сужению.
    if (text.size() > INT32_MAX)
        throw std::runtime_error("Текст превышает 2 ГБ");
    std::vector<std::string> tokens;
    std::string token;
    int32_t position = 0;
    const int32_t length = static_cast<int32_t>(text.size());
    while (position < length) {
        UChar32 codepoint;
        // Шагаем по кодовым точкам, а не по char: кириллическая буква занимает
        // несколько байт. U8_NEXT также обнаруживает обрезанные/неверные последовательности.
        U8_NEXT(reinterpret_cast<const uint8_t*>(text.data()), position, length, codepoint);
        if (codepoint < 0)
            throw std::runtime_error("Некорректный UTF-8, байт " + std::to_string(position));
        const auto category = u_charType(codepoint);
        // Диакритика может быть отдельным символом после буквы: e + ударение.
        // Она продолжает уже начатое слово, но сама по себе словом не становится.
        const bool mark = category == U_NON_SPACING_MARK || category == U_COMBINING_SPACING_MARK ||
                          category == U_ENCLOSING_MARK;
        if (u_isalnum(codepoint) || (mark && !token.empty())) {
            // Простое приведение одной кодовой точки к нижнему регистру не равно
            // стеммингу или полному case folding. После него вновь кодируем UTF-8.
            codepoint = u_tolower(codepoint);
            uint8_t bytes[U8_MAX_LENGTH];
            int32_t count = 0;
            U8_APPEND_UNSAFE(bytes, count, codepoint);
            token.append(reinterpret_cast<const char*>(bytes), static_cast<size_t>(count));
        } else if (!token.empty()) {
            // Пунктуация завершает слово. move передаёт буфер результата без
            // копирования; clear явно возвращает накопитель в пустое состояние.
            tokens.push_back(std::move(token));
            token.clear();
        }
    }
    // Конец текста тоже является границей слова, даже без пробела или точки.
    if (!token.empty())
        tokens.push_back(std::move(token));
    return tokens;
}
std::vector<std::pair<std::string, uint8_t>> TextAnalyzer::analyze(const Document& document) {
    std::vector<std::pair<std::string, uint8_t>> terms;
    auto add = [&](const std::string& text, uint8_t field) {
        for (auto& token : tokenize(text))
            terms.emplace_back(std::move(token), field);
    };
    // Поля анализируются отдельно: конец title и начало headings не должны
    // случайно склеиться в одно слово. URL сохраняется только для выдачи.
    add(document.title, Title);
    add(document.headings, Headings);
    add(document.body, Body);
    std::sort(terms.begin(), terms.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    // Один документ даёт ровно один posting на терм. Маска объединяет поля,
    // сохраняя возможность искать по полю без трёх отдельных индексов.
    size_t used = 0;
    // После сортировки одинаковые строки соседствуют. [0, used) — уже собранный
    // уникальный префикс, i — очередной исходный элемент. Уплотняем на месте,
    // не создавая вторую хеш-таблицу для слов одного документа.
    for (size_t i = 0; i < terms.size(); ++i) {
        if (used && terms[used - 1].first == terms[i].first)
            terms[used - 1].second |= terms[i].second;
        // Самоперемещение строки не требуется: оно могло бы уничтожить её содержимое.
        else {
            if (used != i)
                terms[used] = std::move(terms[i]);
            ++used;
        }
    }
    terms.resize(used);
    return terms;
}
}
