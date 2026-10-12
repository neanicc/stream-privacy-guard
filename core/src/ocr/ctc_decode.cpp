#include "ocr/ctc_decode.hpp"

#include <cmath>
#include <utility>

namespace spg::ocr {

namespace {

// Softmax output can round a little past [0, 1].
constexpr float kProbabilityTolerance = 1e-3F;

}  // namespace

Alphabet Alphabet::from_dictionary(std::string_view dictionary) {
  Alphabet alphabet;
  alphabet.symbols_.emplace_back();  // CTC blank
  std::size_t start = 0;
  std::size_t line = 0;
  while (start < dictionary.size()) {
    std::size_t end = dictionary.find('\n', start);
    if (end == std::string_view::npos) {
      end = dictionary.size();
    }
    std::string_view symbol = dictionary.substr(start, end - start);
    if (!symbol.empty() && symbol.back() == '\r') {
      symbol.remove_suffix(1);
    }
    if (symbol.empty()) {
      throw DecodeError("text recognizer: dictionary line " + std::to_string(line) + " is empty");
    }
    alphabet.symbols_.emplace_back(symbol);
    start = end + 1;
    ++line;
  }
  if (alphabet.symbols_.size() == 1) {
    throw DecodeError("text recognizer: dictionary has no symbols");
  }
  alphabet.symbols_.emplace_back(" ");  // RapidOCR use_space_char
  return alphabet;
}

CtcStats ctc_greedy_decode(std::span<const float> probs, std::size_t steps, const Alphabet& alphabet,
                           std::string& text) {
  const std::size_t classes = alphabet.size();
  if (steps == 0 || probs.size() / steps != classes || probs.size() % steps != 0) {
    throw DecodeError("text recognizer: output has " + std::to_string(probs.size()) + " values, expected " +
                      std::to_string(steps) + " steps x " + std::to_string(classes) + " classes");
  }
  std::string decoded;
  CtcStats stats;
  std::size_t previous = 0;
  for (std::size_t t = 0; t < steps; ++t) {
    const std::span<const float> row = probs.subspan(t * classes, classes);
    std::size_t best = 0;
    for (std::size_t c = 0; c < classes; ++c) {
      const float p = row[c];
      if (!std::isfinite(p) || p < -kProbabilityTolerance || p > 1.0F + kProbabilityTolerance) {
        throw DecodeError("text recognizer: output step " + std::to_string(t) + " class " + std::to_string(c) +
                          " is not a probability");
      }
      if (p > row[best]) {
        best = c;
      }
    }
    if (best != 0 && best != previous) {
      decoded += alphabet.symbol(best);
      stats.confidence_sum += row[best];
      ++stats.characters;
    }
    previous = best;
  }
  text += decoded;
  return stats;
}

CtcResult ctc_greedy_decode(std::span<const float> probs, std::size_t steps, const Alphabet& alphabet) {
  std::string text;
  const CtcStats stats = ctc_greedy_decode(probs, steps, alphabet, text);
  CtcResult result;
  result.text = Secret(std::move(text));
  result.score = stats.characters == 0 ? 0.0F : stats.confidence_sum / static_cast<float>(stats.characters);
  return result;
}

}  // namespace spg::ocr
