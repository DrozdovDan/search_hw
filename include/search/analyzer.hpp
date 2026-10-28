#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "search/storage.hpp"

namespace search {
// Поля — независимые биты: 1|4 означает title и body одновременно. Any=0 служит
// запросу как отсутствие фильтра; сохранённый posting всегда имеет ненулевую маску.
enum Field : uint8_t { Any = 0, Title = 1, Headings = 2, Body = 4 };
class TextAnalyzer {
  public:
    // Буквы и числа Unicode образуют слова; combining marks продолжают слово.
    // Применяется Unicode simple lowercase; диакритика и стоп-слова сохраняются.
    static std::vector<std::string> tokenize(std::string_view text);
    // Возвращает отсортированные уникальные термы документа с масками полей.
    // Частота повторений здесь намеренно теряется: булеву поиску важно наличие.
    // Цена сортировки O(T log T), где T — число исходных токенов во всех полях.
    static std::vector<std::pair<std::string, uint8_t>> analyze(const Document& document);
};
}
