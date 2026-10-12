#include "runtime/onnx_session.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <onnxruntime_cxx_api.h>

namespace spg::runtime {
namespace {

namespace fs = std::filesystem;

struct ProviderNames {
  Provider provider;
  std::string_view spg;  // to_string() and provider_from_string()
  std::string_view ort;  // Ort::GetAvailableProviders() name
};

constexpr std::array<ProviderNames, 4> kProviders{{
    {Provider::Cpu, "cpu", "CPUExecutionProvider"},
    {Provider::CoreMl, "coreml", "CoreMLExecutionProvider"},
    {Provider::DirectMl, "directml", "DmlExecutionProvider"},
    {Provider::Cuda, "cuda", "CUDAExecutionProvider"},
}};

std::string path_text(const fs::path& path) {
  const std::u8string utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

std::string shape_text(std::span<const std::int64_t> shape) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    if (shape[i] < 0) {
      out << '?';
    } else {
      out << shape[i];
    }
  }
  out << ']';
  return out.str();
}

// Element count of `shape` when it fits the model's `spec` (same rank, every
// dimension positive and equal to each fixed spec dimension) and the byte size
// of a float buffer that large is representable; nullopt otherwise. Checked
// before anything is allocated or handed to ONNX Runtime.
std::optional<std::size_t> fitted_count(std::span<const std::int64_t> shape, std::span<const std::int64_t> spec) {
  if (shape.size() != spec.size()) {
    return std::nullopt;
  }
  constexpr std::size_t kMaxCount = std::numeric_limits<std::size_t>::max() / sizeof(float);
  std::size_t count = 1;
  for (std::size_t d = 0; d < shape.size(); ++d) {
    const std::int64_t dim = shape[d];
    if (dim <= 0 || (spec[d] >= 0 && spec[d] != dim)) {
      return std::nullopt;
    }
    // Compared as uint64 before narrowing, so a 32-bit size_t cannot truncate.
    if (static_cast<std::uint64_t>(dim) > kMaxCount / count) {
      return std::nullopt;
    }
    count *= static_cast<std::size_t>(dim);
  }
  return count;
}

std::string provider_list(const std::vector<Provider>& providers) {
  std::string text;
  for (const Provider provider : providers) {
    if (!text.empty()) {
      text += ", ";
    }
    text += to_string(provider);
  }
  return text.empty() ? "none" : text;
}

// Errors that mean the model file itself is unusable, whatever the provider.
bool is_model_error(OrtErrorCode code) {
  return code == ORT_NO_SUCHFILE || code == ORT_NO_MODEL || code == ORT_INVALID_PROTOBUF ||
         code == ORT_INVALID_GRAPH || code == ORT_MODEL_LOADED;
}

OrtLoggingLevel ort_level(Environment::LogLevel level) {
  switch (level) {
    case Environment::LogLevel::Verbose:
      return ORT_LOGGING_LEVEL_VERBOSE;
    case Environment::LogLevel::Info:
      return ORT_LOGGING_LEVEL_INFO;
    case Environment::LogLevel::Warning:
      return ORT_LOGGING_LEVEL_WARNING;
    case Environment::LogLevel::Error:
      return ORT_LOGGING_LEVEL_ERROR;
    case Environment::LogLevel::Fatal:
      return ORT_LOGGING_LEVEL_FATAL;
  }
  return ORT_LOGGING_LEVEL_WARNING;
}

TensorInfo describe(const std::string& name, const Ort::TypeInfo& type) {
  TensorInfo info;
  info.name = name;
  if (type.GetONNXType() == ONNX_TYPE_TENSOR) {
    const auto tensor = type.GetTensorTypeAndShapeInfo();
    info.shape = tensor.GetShape();
    info.float32 = tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  }
  return info;
}

void configure_provider(Ort::SessionOptions& options, Provider provider) {
  switch (provider) {
    case Provider::Cpu:
      return;  // ONNX Runtime always registers CPU last
    case Provider::CoreMl:
      // MLProgram covers more operators than the older NeuralNetwork format;
      // ALL lets Core ML choose between CPU, GPU and Neural Engine.
      // RequireStaticInputShapes keeps nodes with dynamic shapes on CPU.
      options.AppendExecutionProvider("CoreML", {{"ModelFormat", "MLProgram"},
                                                 {"MLComputeUnits", "ALL"},
                                                 {"RequireStaticInputShapes", "1"}});
      return;
    case Provider::DirectMl:
      // The DirectML provider requires these two settings (ONNX Runtime docs).
      options.DisableMemPattern();
      options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
      options.AppendExecutionProvider("DML", {});
      return;
    case Provider::Cuda: {
      const OrtCUDAProviderOptions cuda{};
      options.AppendExecutionProvider_CUDA(cuda);
      return;
    }
  }
}

}  // namespace

std::string_view to_string(Provider provider) noexcept {
  for (const auto& names : kProviders) {
    if (names.provider == provider) {
      return names.spg;
    }
  }
  return "unknown";
}

std::optional<Provider> provider_from_string(std::string_view name) noexcept {
  for (const auto& names : kProviders) {
    if (names.spg == name) {
      return names.provider;
    }
  }
  return std::nullopt;
}

std::vector<Provider> available_providers() {
  std::vector<Provider> providers;
  for (const std::string& ort_name : Ort::GetAvailableProviders()) {
    for (const auto& names : kProviders) {
      if (names.ort == ort_name) {
        providers.push_back(names.provider);
      }
    }
  }
  return providers;
}

std::vector<Provider> default_providers() {
#if defined(__APPLE__)
  return {Provider::CoreMl};
#elif defined(_WIN32)
  return {Provider::DirectMl, Provider::Cuda};
#else
  return {Provider::Cuda};
#endif
}

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ModelNotFound:
      return "model not found";
    case ErrorCode::ModelLoadFailed:
      return "model load failed";
    case ErrorCode::ProviderUnavailable:
      return "provider unavailable";
    case ErrorCode::InvalidInput:
      return "invalid input";
    case ErrorCode::InferenceFailed:
      return "inference failed";
  }
  return "unknown error";
}

RuntimeError::RuntimeError(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

// Environment ------------------------------------------------------------------

struct Environment::Impl {
  Ort::Env env;
};

Environment::Environment(LogLevel level)
    : impl_(std::make_unique<Impl>(Impl{Ort::Env(ort_level(level), "spg")})) {}
Environment::~Environment() = default;
Environment::Environment(Environment&&) noexcept = default;
Environment& Environment::operator=(Environment&&) noexcept = default;

// Session ------------------------------------------------------------------------

struct Session::Impl {
  fs::path path;
  std::string path_label;
  Provider provider = Provider::Cpu;
  std::vector<ProviderAttempt> attempts;
  Ort::Session session{nullptr};
  std::vector<TensorInfo> inputs;
  std::vector<TensorInfo> outputs;
  std::vector<const char*> input_names;
  std::vector<const char*> output_names;

  [[noreturn]] void fail(ErrorCode code, const std::string& what) const {
    throw RuntimeError(code, "spg runtime: " + path_label + ": " + what);
  }

  std::string where() const { return " on provider " + std::string(to_string(provider)); }
};

namespace {

std::unique_ptr<Session::Impl> create_impl(Ort::Env& env, const fs::path& path, Provider provider,
                                           const SessionOptions& options) {
  Ort::SessionOptions ort_options;
  ort_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  if (options.intra_op_threads > 0) {
    ort_options.SetIntraOpNumThreads(options.intra_op_threads);
  }
  configure_provider(ort_options, provider);

  auto impl = std::make_unique<Session::Impl>();
  impl->path = path;
  impl->path_label = path_text(path);
  impl->provider = provider;
  impl->session = Ort::Session(env, path.c_str(), ort_options);

  const Ort::AllocatorWithDefaultOptions allocator;
  const std::size_t input_count = impl->session.GetInputCount();
  for (std::size_t i = 0; i < input_count; ++i) {
    const std::string name = impl->session.GetInputNameAllocated(i, allocator).get();
    impl->inputs.push_back(describe(name, impl->session.GetInputTypeInfo(i)));
  }
  const std::size_t output_count = impl->session.GetOutputCount();
  for (std::size_t i = 0; i < output_count; ++i) {
    const std::string name = impl->session.GetOutputNameAllocated(i, allocator).get();
    impl->outputs.push_back(describe(name, impl->session.GetOutputTypeInfo(i)));
  }
  // Pointers into the TensorInfo strings stay valid: the vectors are final.
  for (const TensorInfo& info : impl->inputs) {
    impl->input_names.push_back(info.name.c_str());
  }
  for (const TensorInfo& info : impl->outputs) {
    impl->output_names.push_back(info.name.c_str());
  }
  return impl;
}

// A moved-from Session has no Impl. Calls that can throw fail closed with an
// error instead of dereferencing null.
const Session::Impl& live(const std::unique_ptr<Session::Impl>& impl) {
  if (!impl) {
    throw RuntimeError(ErrorCode::InvalidInput, "spg runtime: this Session was moved from and holds no model");
  }
  return *impl;
}

}  // namespace

Session::Session(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

Session Session::open(Environment& env, const std::filesystem::path& model, const SessionOptions& options) {
  const std::string label = path_text(model);
  if (!env.impl_) {
    throw RuntimeError(ErrorCode::InvalidInput,
                       "spg runtime: " + label + ": the Environment was moved from; open sessions with a live one");
  }
  std::error_code ec;
  if (!fs::is_regular_file(model, ec)) {
    throw RuntimeError(ErrorCode::ModelNotFound,
                       "spg runtime: model file not found: " + label +
                           " (run scripts/fetch_models.py, or check the configured model directory)");
  }

  std::vector<Provider> order = options.providers;
  if (options.cpu_fallback && std::find(order.begin(), order.end(), Provider::Cpu) == order.end()) {
    order.push_back(Provider::Cpu);
  }
  const std::vector<Provider> available = available_providers();

  std::vector<ProviderAttempt> attempts;
  bool model_error = false;
  // Set when CPU itself failed: the model, not an accelerator, is the problem.
  std::optional<ErrorCode> cpu_error;
  for (const Provider provider : order) {
    if (std::find(available.begin(), available.end(), provider) == available.end()) {
      attempts.push_back({provider, false,
                          "not in this ONNX Runtime build (available: " + provider_list(available) + ")"});
      continue;
    }
    ErrorCode code = ErrorCode::ModelLoadFailed;
    try {
      auto impl = create_impl(env.impl_->env, model, provider, options);
      Session session(std::move(impl));
      if (options.warmup_runs > 0) {
        session.warmup(options.warmup_shapes, options.warmup_runs);
      }
      attempts.push_back({provider, true, {}});
      session.impl_->attempts = std::move(attempts);
      return session;
    } catch (const RuntimeError& error) {
      if (error.code() == ErrorCode::InvalidInput) {
        throw;  // wrong warmup shapes fail the same way on every provider
      }
      code = error.code();
      attempts.push_back({provider, false, error.what()});
    } catch (const Ort::Exception& error) {
      model_error = model_error || is_model_error(error.GetOrtErrorCode());
      attempts.push_back({provider, false, error.what()});
    }
    if (provider == Provider::Cpu) {
      cpu_error = code;
    }
  }

  std::string detail;
  for (const ProviderAttempt& attempt : attempts) {
    detail += "\n  " + std::string(to_string(attempt.provider)) + ": " + attempt.detail;
  }
  if (model_error || cpu_error == ErrorCode::ModelLoadFailed) {
    throw RuntimeError(ErrorCode::ModelLoadFailed,
                       "spg runtime: " + label + ": ONNX Runtime could not load the model" + detail);
  }
  if (cpu_error) {
    throw RuntimeError(*cpu_error, "spg runtime: " + label + ": warmup failed on every provider" + detail);
  }
  throw RuntimeError(ErrorCode::ProviderUnavailable,
                     "spg runtime: " + label + ": no requested execution provider worked (requested: " +
                         provider_list(options.providers) + ", CPU fallback " +
                         (options.cpu_fallback ? "on" : "off") + ")" + detail);
}

Provider Session::provider() const { return live(impl_).provider; }
const std::vector<ProviderAttempt>& Session::attempts() const { return live(impl_).attempts; }
const std::filesystem::path& Session::model_path() const { return live(impl_).path; }
const std::vector<TensorInfo>& Session::inputs() const { return live(impl_).inputs; }
const std::vector<TensorInfo>& Session::outputs() const { return live(impl_).outputs; }

std::optional<std::string> Session::metadata(std::string_view key) const {
  const Impl& impl = live(impl_);
  const Ort::AllocatorWithDefaultOptions allocator;
  const Ort::ModelMetadata meta = impl.session.GetModelMetadata();
  const Ort::AllocatedStringPtr value = meta.LookupCustomMetadataMapAllocated(std::string(key).c_str(), allocator);
  if (!value) {
    return std::nullopt;
  }
  return std::string(value.get());
}

std::vector<Tensor> Session::run(std::span<const TensorView> inputs) const {
  const Impl& impl = live(impl_);
  if (inputs.size() != impl.inputs.size()) {
    impl.fail(ErrorCode::InvalidInput, "expected " + std::to_string(impl.inputs.size()) + " input tensor(s), got " +
                                           std::to_string(inputs.size()));
  }

  const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<Ort::Value> values;
  values.reserve(inputs.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const TensorInfo& spec = impl.inputs[i];
    const TensorView& view = inputs[i];
    const std::string prefix = "input '" + spec.name + "' ";
    if (!spec.float32) {
      impl.fail(ErrorCode::InvalidInput, prefix + "is not a float32 tensor; only float32 is supported");
    }
    const std::optional<std::size_t> fitted = fitted_count(view.shape, spec.shape);
    if (!fitted) {
      impl.fail(ErrorCode::InvalidInput,
                prefix + "has shape " + shape_text(view.shape) + ", model expects " + shape_text(spec.shape));
    }
    const std::size_t count = *fitted;
    if (view.data.size() != count) {
      impl.fail(ErrorCode::InvalidInput, prefix + "shape " + shape_text(view.shape) + " needs " +
                                             std::to_string(count) + " values, got " +
                                             std::to_string(view.data.size()));
    }
    // ONNX Runtime only reads input buffers; the cast satisfies its C signature.
    values.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(view.data.data()), count,
                                                     view.shape.data(), view.shape.size()));
  }

  std::vector<Ort::Value> results;
  try {
    results = impl_->session.Run(Ort::RunOptions{nullptr}, impl.input_names.data(), values.data(), values.size(),
                                 impl.output_names.data(), impl.output_names.size());
  } catch (const Ort::Exception& error) {
    impl.fail(ErrorCode::InferenceFailed, "inference failed" + impl.where() + ": " + error.what());
  }

  std::vector<Tensor> outputs;
  outputs.reserve(results.size());
  for (std::size_t i = 0; i < results.size(); ++i) {
    const Ort::Value& value = results[i];
    if (!value.IsTensor() ||
        value.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      impl.fail(ErrorCode::InferenceFailed,
                "output '" + impl.outputs[i].name + "' is not a float32 tensor; only float32 is supported");
    }
    const auto info = value.GetTensorTypeAndShapeInfo();
    Tensor tensor;
    tensor.shape = info.GetShape();
    const float* data = value.GetTensorData<float>();
    tensor.data.assign(data, data + info.GetElementCount());
    outputs.push_back(std::move(tensor));
  }
  return outputs;
}

void Session::warmup(std::span<const std::vector<std::int64_t>> shapes, int runs) const {
  const Impl& impl = live(impl_);
  std::vector<std::vector<std::int64_t>> resolved;
  if (shapes.empty()) {
    for (const TensorInfo& spec : impl.inputs) {
      if (std::any_of(spec.shape.begin(), spec.shape.end(), [](std::int64_t dim) { return dim < 0; })) {
        impl.fail(ErrorCode::InvalidInput, "input '" + spec.name + "' has dynamic shape " + shape_text(spec.shape) +
                                               "; give a concrete warmup shape (SessionOptions::warmup_shapes)");
      }
      resolved.push_back(spec.shape);
    }
  } else {
    if (shapes.size() != impl.inputs.size()) {
      impl.fail(ErrorCode::InvalidInput, "expected " + std::to_string(impl.inputs.size()) +
                                             " warmup shape(s), got " + std::to_string(shapes.size()));
    }
    resolved.assign(shapes.begin(), shapes.end());
  }

  std::vector<std::vector<float>> buffers;
  std::vector<TensorView> views;
  buffers.reserve(resolved.size());
  // Check every shape before allocating any buffer, so a wrong or huge warmup
  // shape is an InvalidInput error rather than a giant allocation.
  std::vector<std::size_t> counts;
  counts.reserve(resolved.size());
  for (std::size_t i = 0; i < resolved.size(); ++i) {
    const TensorInfo& spec = impl.inputs[i];
    const std::optional<std::size_t> count = fitted_count(resolved[i], spec.shape);
    if (!count) {
      impl.fail(ErrorCode::InvalidInput, "warmup shape " + shape_text(resolved[i]) + " for input '" + spec.name +
                                             "' does not fit the model, which expects " + shape_text(spec.shape));
    }
    counts.push_back(*count);
  }
  for (std::size_t i = 0; i < resolved.size(); ++i) {
    buffers.emplace_back(counts[i], 0.0F);
    views.push_back({buffers.back(), resolved[i]});
  }
  for (int i = 0; i < runs; ++i) {
    static_cast<void>(run(views));
  }
}

}  // namespace spg::runtime
