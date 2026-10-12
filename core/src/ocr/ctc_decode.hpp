#pragma once

// CTC decoding for the PP-OCR text recognizer (SPG-07, lane:vision). Pure
// functions over plain float spans, tested with synthetic probabilities; no
// model needed.
//
// Follows RapidOCR v3.9.2 CTCLabelDecode (python/rapidocr/ch_ppocr_rec/
// utils.py): the alphabet is the model's 'character' metadata split into
// lines, with the CTC blank inserted at index 0 and a space appended
// (use_space_char). Decoding is greedy: the most probable class per time step,
// repeats collapsed, blanks dropped. Unlike RapidOCR nothing is dropped for a
// low score (its text_score 0.5 cut): a misread secret still goes to the
// policy, which decides (recall over precision).
//
// Decoded text is secret-adjacent: it is returned as spg::Secret and never
// appears in an error message. Errors name sizes, steps and classes only.

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "spg/secret.hpp"

namespace spg::ocr {

// Thrown when model output is malformed (wrong size, a value that is not a
// finite probability) or the alphabet is unusable. The caller fails the frame
// closed. Messages carry sizes and indices, never text or tensor values.
class DecodeError : public std::runtime_error {
 public:
  explicit DecodeError(const std::string& what) : std::runtime_error(what) {}
};

// The CTC classes: 0 is the blank, then one symbol per dictionary line, then
// a space.
class Alphabet {
 public:
  // Splits `dictionary` on '\n' (a trailing '\r' on a line is removed, and the
  // empty segment after a final newline is ignored). Throws DecodeError when
  // there is no symbol, or a line in the middle is empty.
  [[nodiscard]] static Alphabet from_dictionary(std::string_view dictionary);

  // Number of classes, blank and space included.
  [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }
  // UTF-8 text of class `index`; empty for the blank (index 0).
  [[nodiscard]] std::string_view symbol(std::size_t index) const { return symbols_.at(index); }

 private:
  std::vector<std::string> symbols_;
};

// Summary of one decoded sequence.
struct CtcStats {
  float confidence_sum = 0.0F;  // sum of the kept steps' probabilities
  std::size_t characters = 0;   // kept steps (symbols appended)
};

// Greedy-decodes one sequence: `probs` holds `steps` rows of alphabet.size()
// probabilities, row-major. Appends the text to `text` and returns its stats.
// Throws DecodeError when the size does not match or any value is not a
// finite number in [0, 1] (a small rounding tolerance is allowed); `text` is
// unchanged then.
CtcStats ctc_greedy_decode(std::span<const float> probs, std::size_t steps, const Alphabet& alphabet,
                           std::string& text);

// Convenience form for tests and callers that decode one sequence: the text as
// a Secret and the mean probability of its characters (0 when empty).
struct CtcResult {
  Secret text;
  float score = 0.0F;
};
[[nodiscard]] CtcResult ctc_greedy_decode(std::span<const float> probs, std::size_t steps,
                                          const Alphabet& alphabet);

}  // namespace spg::ocr
