#pragma once

// ONNX Runtime session wrapper for spg-core detectors (lane:vision).
//
// Consumers never include ONNX Runtime headers: this header is the whole API.
// A Session is created once per model, tries execution providers in the
// caller's order, falls back to CPU unless told not to, warms up before it is
// returned, and reports every failure as a RuntimeError with a code and a
// message naming the model, the provider and what went wrong. Models are read
// from local files, or from bytes the caller already read and checked;
// nothing here touches the network.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace spg::runtime {

// Execution providers SPG knows how to configure.
enum class Provider {
  Cpu,       // always present in every ONNX Runtime build
  CoreMl,    // macOS; part of the pinned osx-arm64 package
  DirectMl,  // Windows; needs the DirectML ONNX Runtime package (not pinned yet)
  Cuda,      // Windows/Linux; needs the CUDA ONNX Runtime package and CUDA/cuDNN
};

[[nodiscard]] std::string_view to_string(Provider provider) noexcept;
// Accepts the names to_string() returns ("cpu", "coreml", "directml", "cuda").
[[nodiscard]] std::optional<Provider> provider_from_string(std::string_view name) noexcept;

// Providers compiled into the ONNX Runtime library loaded by this process.
// Being listed does not prove the device works; Session::open() checks that.
[[nodiscard]] std::vector<Provider> available_providers();

// Platform default order, CPU excluded (CPU is the fallback, see SessionOptions):
// macOS {CoreMl}; Windows {DirectMl, Cuda}; Linux {Cuda}.
[[nodiscard]] std::vector<Provider> default_providers();

enum class ErrorCode {
  ModelNotFound,        // the model path does not name a readable file
  ModelLoadFailed,      // ONNX Runtime rejected the file (corrupt, wrong format, unsupported op)
  ProviderUnavailable,  // no requested provider worked and CPU fallback was disabled
  InvalidInput,         // input count, shape or warmup shape does not fit the model
  InferenceFailed,      // ONNX Runtime failed while running a valid request
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Messages carry model paths, provider names, tensor names and shapes. They
// never carry tensor contents.
class RuntimeError : public std::runtime_error {
 public:
  RuntimeError(ErrorCode code, const std::string& message);
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

// Owns the process-wide ONNX Runtime environment (its logger and thread
// state). Create one per process and keep it alive longer than every Session
// opened with it.
class Environment {
 public:
  enum class LogLevel { Verbose, Info, Warning, Error, Fatal };

  explicit Environment(LogLevel level = LogLevel::Warning);
  ~Environment();
  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;
  Environment(Environment&&) noexcept;
  Environment& operator=(Environment&&) noexcept;

  struct Impl;

 private:
  friend class Session;
  std::unique_ptr<Impl> impl_;
};

struct SessionOptions {
  // Tried in order. Providers missing from this build are skipped and recorded.
  std::vector<Provider> providers = default_providers();
  // Use CPU when every provider above fails. Disable it only to test or
  // require a specific accelerator; the pipeline fails closed on the error.
  bool cpu_fallback = true;
  // ONNX Runtime intra-op threads; 0 keeps its default (one per physical core).
  int intra_op_threads = 0;
  // Runs before open() returns. A provider that cannot complete them counts as
  // failed, so a broken accelerator falls back to CPU here, not mid-stream.
  int warmup_runs = 1;
  // One concrete shape per model input, in model input order. May be empty
  // when every input shape is fully static.
  std::vector<std::vector<std::int64_t>> warmup_shapes;
};

// Float32 tensor description. Dynamic dimensions are -1.
struct TensorInfo {
  std::string name;
  std::vector<std::int64_t> shape;
  bool float32 = false;  // only float32 inputs and outputs are supported
};

// Non-owning view of one input tensor.
struct TensorView {
  std::span<const float> data;
  std::span<const std::int64_t> shape;
};

struct Tensor {
  std::vector<std::int64_t> shape;
  std::vector<float> data;
};

// What happened to each provider open() considered, in order.
struct ProviderAttempt {
  Provider provider;
  bool selected = false;
  std::string detail;  // why it was skipped or failed; empty when selected
};

// A model already in memory: for a caller that checked the bytes (size,
// SHA-256) and must not let ONNX Runtime read the file again, where it could
// have been replaced since. `path` only names the model in errors and
// model_path(); the file is not read. The bytes need to outlive open() only.
struct ModelBytes {
  std::filesystem::path path;
  std::span<const std::byte> bytes;
};

class Session {
 public:
  // Throws RuntimeError: ModelNotFound, ModelLoadFailed, ProviderUnavailable or
  // InvalidInput (bad warmup shapes, or `env` was moved from).
  [[nodiscard]] static Session open(Environment& env, const std::filesystem::path& model,
                                    const SessionOptions& options = {});
  // The same from memory. Empty or unparsable bytes are ModelLoadFailed.
  [[nodiscard]] static Session open(Environment& env, const ModelBytes& model, const SessionOptions& options = {});

  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) noexcept;
  Session& operator=(Session&&) noexcept;

  // A moved-from Session holds no model: destroy it or assign to it. Every
  // other member function on it throws RuntimeError InvalidInput.

  // The provider the session was created with. ONNX Runtime may still run
  // individual nodes the provider does not support on CPU.
  [[nodiscard]] Provider provider() const;
  [[nodiscard]] const std::vector<ProviderAttempt>& attempts() const;
  [[nodiscard]] const std::filesystem::path& model_path() const;
  [[nodiscard]] const std::vector<TensorInfo>& inputs() const;
  [[nodiscard]] const std::vector<TensorInfo>& outputs() const;
  // Custom metadata stored in the model, for example a recognizer's dictionary.
  [[nodiscard]] std::optional<std::string> metadata(std::string_view key) const;

  // One tensor per model input, in order; returns every output in model order.
  // Safe to call from several threads at once. Throws RuntimeError
  // InvalidInput or InferenceFailed.
  [[nodiscard]] std::vector<Tensor> run(std::span<const TensorView> inputs) const;

  // Runs `runs` inferences on zero-filled inputs of the given shapes (see
  // SessionOptions::warmup_shapes). Shapes that do not fit the model are
  // rejected with InvalidInput before any buffer is allocated. Throws like run().
  void warmup(std::span<const std::vector<std::int64_t>> shapes, int runs = 1) const;

  struct Impl;

 private:
  explicit Session(std::unique_ptr<Impl> impl);
  // Both open()s: `bytes` when set, else the file at `model`.
  [[nodiscard]] static Session open_from(Environment& env, const std::filesystem::path& model,
                                         const std::optional<std::span<const std::byte>>& bytes,
                                         const SessionOptions& options);
  std::unique_ptr<Impl> impl_;
};

}  // namespace spg::runtime
