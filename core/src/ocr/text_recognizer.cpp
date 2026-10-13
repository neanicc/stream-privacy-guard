#include "ocr/text_recognizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>

#include "ocr/run_batches.hpp"
#include "spg/error.hpp"
#include "spg/secure_memory.hpp"

namespace spg::ocr {

namespace {

constexpr std::uint32_t kMaxInputHeight = 1024;
constexpr std::uint32_t kMaxStride = 64;
constexpr std::uint32_t kMaxWidth = 1U << 14;
constexpr std::size_t kMaxBatch = 64;
constexpr std::size_t kMaxParallelRuns = 16;

std::string shape_text(std::span<const std::int64_t> shape) {
  std::string text = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    text += (i == 0 ? "" : ", ") + (shape[i] < 0 ? std::string("?") : std::to_string(shape[i]));
  }
  return text + "]";
}

// Whether `dim` (possibly dynamic, -1) can take `want`.
bool fits(std::int64_t dim, std::int64_t want) {
  return dim < 0 || dim == want;
}

Alphabet load_alphabet(const runtime::Session& session) {
  const std::optional<std::string> dictionary = session.metadata("character");
  if (!dictionary) {
    throw TextRecognizerError("text recognizer: model has no 'character' metadata (the CTC dictionary)");
  }
  try {
    return Alphabet::from_dictionary(*dictionary);
  } catch (const DecodeError& error) {
    throw TextRecognizerError(error.what());
  }
}

void check_model(const runtime::Session& session, const TextRecognizerConfig& config, const Alphabet& alphabet) {
  const auto& inputs = session.inputs();
  if (inputs.size() != 1 || !inputs[0].float32) {
    throw TextRecognizerError("text recognizer: expected one float32 input, model has " +
                              std::to_string(inputs.size()) + " input(s)");
  }
  const auto& in = inputs[0].shape;
  const bool batch_ok = in.empty() || in[0] < 0 || (in[0] == 1 && config.max_batch == 1);
  if (!in.empty() && (in.size() != 4 || !batch_ok || !fits(in[1], 3) ||
                      !fits(in[2], static_cast<std::int64_t>(config.input_height)) || in[3] >= 0)) {
    throw TextRecognizerError("text recognizer: input '" + inputs[0].name + "' has shape " + shape_text(in) +
                              ", expected [N, 3, " + std::to_string(config.input_height) +
                              ", W] with a dynamic width (and a dynamic N when max_batch > 1)");
  }
  const auto& outputs = session.outputs();
  if (outputs.size() != 1 || !outputs[0].float32) {
    throw TextRecognizerError("text recognizer: expected one float32 output, model has " +
                              std::to_string(outputs.size()) + " output(s)");
  }
  const auto& out = outputs[0].shape;
  if (!out.empty() && (out.size() != 3 || !fits(out[2], static_cast<std::int64_t>(alphabet.size())))) {
    throw TextRecognizerError("text recognizer: output '" + outputs[0].name + "' has shape " + shape_text(out) +
                              ", expected [N, T, " + std::to_string(alphabet.size()) +
                              "] (dictionary lines + blank + space); is it a CTC recognizer?");
  }
}

ViewParams view_params(const TextRecognizerConfig& config) {
  return {config.input_height, config.max_width, config.chunk_overlap, config.tall_ratio};
}

std::uint32_t round_up(std::uint32_t value, std::uint32_t step) {
  return (value + step - 1) / step * step;
}

// The image one read job feeds the model: input_height tall, job.width wide.
detect::BgrImage render_job(const FrameView& frame, const ReadJob& job, std::uint32_t input_height) {
  const Rect crop{static_cast<float>(job.rect.x), static_cast<float>(job.rect.y),
                  static_cast<float>(job.rect.width), static_cast<float>(job.rect.height)};
  if (job.rotation == Rotation::None) {
    return detect::render_model_input(frame, ModelInputMapping(crop, Size{job.width, input_height}, Fit::Stretch));
  }
  // Scale with the reading direction along y, then turn it horizontal.
  const detect::BgrImage upright =
      detect::render_model_input(frame, ModelInputMapping(crop, Size{input_height, job.width}, Fit::Stretch));
  return rotate(upright, job.rotation);
}

// Zeroes the model's output when it goes out of scope, decoded or not: the
// most probable class per step spells the text.
class WipeOutputs {
 public:
  explicit WipeOutputs(std::vector<runtime::Tensor>& outputs) noexcept : outputs_(outputs) {}
  ~WipeOutputs() {
    for (runtime::Tensor& output : outputs_) {
      secure_wipe(output.data.data(), output.data.size() * sizeof(float));
    }
  }
  WipeOutputs(const WipeOutputs&) = delete;
  WipeOutputs& operator=(const WipeOutputs&) = delete;

 private:
  std::vector<runtime::Tensor>& outputs_;
};

}  // namespace

void validate(const TextRecognizerConfig& config) {
  if (config.input_height < 8 || config.input_height > kMaxInputHeight) {
    throw TextRecognizerError("text recognizer: input_height must be in [8, 1024]");
  }
  if (config.time_stride < 1 || config.time_stride > kMaxStride) {
    throw TextRecognizerError("text recognizer: time_stride must be in [1, 64]");
  }
  if (config.min_width < config.time_stride || config.min_width % config.time_stride != 0) {
    throw TextRecognizerError("text recognizer: min_width must be a positive multiple of time_stride");
  }
  if (config.max_width < config.min_width || config.max_width > kMaxWidth ||
      config.max_width % config.time_stride != 0) {
    throw TextRecognizerError(
        "text recognizer: max_width must be a multiple of time_stride in [min_width, 16384]");
  }
  if (!(config.chunk_overlap >= 0.0F && config.chunk_overlap <= 0.5F)) {
    throw TextRecognizerError("text recognizer: chunk_overlap must be in [0, 0.5]");
  }
  if (!(std::isfinite(config.tall_ratio) && config.tall_ratio >= 1.0F)) {
    throw TextRecognizerError("text recognizer: tall_ratio must be finite and at least 1");
  }
  if (config.max_batch < 1 || config.max_batch > kMaxBatch) {
    throw TextRecognizerError("text recognizer: max_batch must be in [1, 64]");
  }
  if (config.parallel_runs < 1 || config.parallel_runs > kMaxParallelRuns) {
    throw TextRecognizerError("text recognizer: parallel_runs must be in [1, 16]");
  }
  try {
    validate(config.lines);
  } catch (const std::invalid_argument& error) {
    throw TextRecognizerError(std::string("text recognizer: ") + error.what());
  }
}

PixelRect readback_crop(const Box& box, const ReadbackMapping& readback) {
  if (box.space != Space::Source) {
    throw ContractError("text recognizer: region box must be in Source space");
  }
  const Box converted = convert(box, Space::Readback, SpaceMap{readback, std::nullopt});
  return enclosing_pixels(converted.rect, readback.readback());
}

std::vector<float> to_recognizer_tensor(std::span<const detect::BgrImage> images, std::uint32_t input_height,
                                        std::uint32_t width) {
  const std::size_t plane = static_cast<std::size_t>(input_height) * width;
  std::vector<float> tensor(images.size() * 3 * plane, 0.0F);
  for (std::size_t n = 0; n < images.size(); ++n) {
    const detect::BgrImage& image = images[n];
    const std::uint32_t w = image.size.width;
    if (image.size.height != input_height || w > width ||
        image.pixels.size() != 3 * static_cast<std::size_t>(w) * input_height) {
      throw ContractError("text recognizer: crop image does not fit the batch tensor");
    }
    float* base = tensor.data() + n * 3 * plane;
    for (std::uint32_t y = 0; y < input_height; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::size_t from = 3 * (static_cast<std::size_t>(y) * w + x);
        const std::size_t to = static_cast<std::size_t>(y) * width + x;
        for (std::size_t c = 0; c < 3; ++c) {
          base[c * plane + to] = static_cast<float>(image.pixels[from + c]) / 127.5F - 1.0F;
        }
      }
    }
  }
  return tensor;
}

namespace {

// Validates `config` before paying for a session and gives a default warmup.
runtime::SessionOptions recognizer_options(const TextRecognizerConfig& config, runtime::SessionOptions options) {
  validate(config);
  if (options.warmup_shapes.empty()) {
    options.warmup_shapes = {{1, 3, static_cast<std::int64_t>(config.input_height),
                              static_cast<std::int64_t>(config.min_width)}};
  }
  return options;
}

}  // namespace

TextRecognizer TextRecognizer::open(runtime::Environment& env, const std::filesystem::path& model,
                                    const TextRecognizerConfig& config, runtime::SessionOptions options) {
  options = recognizer_options(config, std::move(options));
  return TextRecognizer(runtime::Session::open(env, model, options), config);
}

TextRecognizer TextRecognizer::open(runtime::Environment& env, const runtime::ModelBytes& model,
                                    const TextRecognizerConfig& config, runtime::SessionOptions options) {
  options = recognizer_options(config, std::move(options));
  return TextRecognizer(runtime::Session::open(env, model, options), config);
}

TextRecognizer::TextRecognizer(runtime::Session session, const TextRecognizerConfig& config)
    : session_(std::move(session)), config_(config) {
  validate(config_);
  alphabet_ = load_alphabet(session_);
  check_model(session_, config_, alphabet_);
}

std::vector<Recognition> TextRecognizer::recognize(const FrameView& frame, Size source,
                                                   std::span<const Detection> regions) const {
  validate(frame);
  const ReadbackMapping readback(source, frame.size());
  const std::uint32_t height = config_.input_height;
  const ViewParams params = view_params(config_);

  // Every read job of every region, in plan order per region.
  struct Job {
    ReadJob read;
    std::size_t region = 0;
  };
  std::vector<Job> jobs;
  for (std::size_t r = 0; r < regions.size(); ++r) {
    validate(regions[r]);
    const PixelRect crop = readback_crop(regions[r].box, readback);
    if (crop.width <= 0 || crop.height <= 0) {
      continue;
    }
    const auto w = static_cast<std::uint32_t>(crop.width);
    const auto h = static_cast<std::uint32_t>(crop.height);
    const Rect rect{static_cast<float>(crop.x), static_cast<float>(crop.y), static_cast<float>(w),
                    static_cast<float>(h)};
    const detect::BgrImage pixels =
        detect::render_model_input(frame, ModelInputMapping(rect, Size{w, h}, Fit::Stretch));
    for (const ReadJob& read : plan_reads(crop, split_lines(pixels, config_.lines), params)) {
      jobs.push_back({read, r});
    }
  }

  // Run jobs in width order so a batch pads little; decode into per-job text.
  // Every buffer that holds recognized text is a SecureText (protected memory,
  // zeroed when released); no std::string holds any of it.
  std::vector<std::size_t> order(jobs.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return jobs[a].read.width < jobs[b].read.width; });
  std::vector<SecureText> texts(jobs.size());
  std::vector<CtcStats> stats(jobs.size());
  const std::size_t classes = alphabet_.size();
  // One model run: the batch of jobs order[first, first + count). Batches
  // write disjoint entries of texts and stats, so they can run at once.
  const auto run_batch = [&](std::size_t first) {
    const std::size_t count = std::min(config_.max_batch, order.size() - first);
    std::vector<detect::BgrImage> images;
    images.reserve(count);
    std::uint32_t widest = config_.min_width;
    for (std::size_t k = 0; k < count; ++k) {
      const ReadJob& read = jobs[order[first + k]].read;
      images.push_back(render_job(frame, read, height));
      widest = std::max(widest, read.width);
    }
    const std::uint32_t width = round_up(widest, config_.time_stride);
    const std::vector<float> tensor = to_recognizer_tensor(images, height, width);
    const std::array<std::int64_t, 4> shape{static_cast<std::int64_t>(count), 3, static_cast<std::int64_t>(height),
                                            static_cast<std::int64_t>(width)};
    const std::array<runtime::TensorView, 1> inputs{{{tensor, shape}}};
    std::vector<runtime::Tensor> outputs = session_.run(inputs);
    const WipeOutputs wipe_outputs(outputs);
    const std::size_t steps = width / config_.time_stride;
    const std::array<std::int64_t, 3> want{static_cast<std::int64_t>(count), static_cast<std::int64_t>(steps),
                                           static_cast<std::int64_t>(classes)};
    if (outputs.size() != 1 ||
        !std::equal(outputs[0].shape.begin(), outputs[0].shape.end(), want.begin(), want.end()) ||
        outputs[0].data.size() != count * steps * classes) {
      throw DecodeError("text recognizer: output has shape " +
                        (outputs.empty() ? std::string("(none)") : shape_text(outputs[0].shape)) + ", expected " +
                        shape_text(want));
    }
    const std::span<const float> all(outputs[0].data);
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t j = order[first + k];
      stats[j] = ctc_greedy_decode(all.subspan(k * steps * classes, steps * classes), steps, alphabet_, texts[j]);
    }
  };
  // Widest batches first, so the lanes finish close together.
  const std::size_t batches = (order.size() + config_.max_batch - 1) / config_.max_batch;
  run_batches(batches, config_.parallel_runs,
              [&](std::size_t b) { run_batch((batches - 1 - b) * config_.max_batch); });

  // Join each region's texts: chunks of one view by ' ', views by '\n'.
  std::vector<SecureText> joined(regions.size());
  std::vector<CtcStats> totals(regions.size());
  std::vector<std::size_t> last_view(regions.size(), static_cast<std::size_t>(-1));
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    const std::size_t r = jobs[j].region;
    totals[r].confidence_sum += stats[j].confidence_sum;
    totals[r].characters += stats[j].characters;
    if (texts[j].empty()) {
      continue;
    }
    if (!joined[r].empty()) {
      joined[r].push_back(jobs[j].read.view == last_view[r] ? ' ' : '\n');
    }
    joined[r].insert(joined[r].end(), texts[j].begin(), texts[j].end());
    last_view[r] = jobs[j].read.view;
  }
  std::vector<Recognition> results(regions.size());
  for (std::size_t r = 0; r < regions.size(); ++r) {
    results[r].text = Secret::copy_of(view(joined[r]));
    results[r].score = totals[r].characters == 0
                           ? 0.0F
                           : totals[r].confidence_sum / static_cast<float>(totals[r].characters);
  }
  return results;
}

}  // namespace spg::ocr
