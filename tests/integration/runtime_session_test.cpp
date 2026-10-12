// spg::runtime::Session against tiny models generated at build time
// (make_runtime_fixtures.py) and, when scripts/fetch_models.py has run, the
// models pinned in models/manifest.json.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "runtime/onnx_session.hpp"

namespace {

namespace fs = std::filesystem;
using spg::runtime::Environment;
using spg::runtime::ErrorCode;
using spg::runtime::Provider;
using spg::runtime::RuntimeError;
using spg::runtime::Session;
using spg::runtime::SessionOptions;
using spg::runtime::Tensor;
using spg::runtime::TensorView;
using Catch::Matchers::ContainsSubstring;

fs::path fixture(std::string_view name) { return fs::path(SPG_RUNTIME_FIXTURES) / name; }

// One ONNX Runtime environment per process, as the API asks.
Environment& shared_env() {
  static Environment env(Environment::LogLevel::Error);
  return env;
}

bool contains(const std::vector<Provider>& providers, Provider provider) {
  return std::find(providers.begin(), providers.end(), provider) != providers.end();
}

SessionOptions cpu_only() {
  SessionOptions options;
  options.providers = {Provider::Cpu};
  options.cpu_fallback = false;
  return options;
}

// The first default provider this build offers, else CPU: what open() must pick.
Provider expected_default_provider() {
  const auto available = spg::runtime::available_providers();
  for (const Provider provider : spg::runtime::default_providers()) {
    if (contains(available, provider)) {
      return provider;
    }
  }
  return Provider::Cpu;
}

std::vector<float> doubled(const Session& session, const std::vector<float>& values,
                           const std::vector<std::int64_t>& shape) {
  const TensorView view{values, shape};
  const std::vector<Tensor> out = session.run({&view, 1});
  REQUIRE(out.size() == 1);
  CHECK(out[0].shape == shape);
  return out[0].data;
}

std::vector<float> times_two(std::vector<float> values) {
  for (float& value : values) {
    value *= 2.0F;
  }
  return values;
}

// Runs `fn`, requires a RuntimeError with `code`, and returns its message.
template <typename Fn>
std::string error_of(ErrorCode code, Fn&& fn) {
  try {
    fn();
  } catch (const RuntimeError& error) {
    CHECK(spg::runtime::to_string(error.code()) == spg::runtime::to_string(code));
    return error.what();
  }
  FAIL("expected RuntimeError: " << spg::runtime::to_string(code));
  return {};
}

}  // namespace

TEST_CASE("Provider names round-trip and defaults exclude CPU", "[runtime]") {
  for (const Provider provider : {Provider::Cpu, Provider::CoreMl, Provider::DirectMl, Provider::Cuda}) {
    CHECK(spg::runtime::provider_from_string(spg::runtime::to_string(provider)) == provider);
  }
  CHECK_FALSE(spg::runtime::provider_from_string("gpu").has_value());
  CHECK_FALSE(contains(spg::runtime::default_providers(), Provider::Cpu));
#if defined(__APPLE__)
  CHECK(spg::runtime::default_providers() == std::vector<Provider>{Provider::CoreMl});
#endif
}

TEST_CASE("The pinned ONNX Runtime offers CPU, and CoreML on macOS", "[runtime]") {
  const auto available = spg::runtime::available_providers();
  CHECK(contains(available, Provider::Cpu));
#if defined(__APPLE__)
  CHECK(contains(available, Provider::CoreMl));
#endif
}

TEST_CASE("A static model warms up and runs on CPU", "[runtime]") {
  const Session session = Session::open(shared_env(), fixture("scale_static.onnx"), cpu_only());

  CHECK(session.provider() == Provider::Cpu);
  REQUIRE(session.attempts().size() == 1);
  CHECK(session.attempts()[0].selected);
  CHECK(session.model_path() == fixture("scale_static.onnx"));
  REQUIRE(session.inputs().size() == 1);
  CHECK(session.inputs()[0].name == "x");
  CHECK(session.inputs()[0].shape == std::vector<std::int64_t>{2, 3});
  CHECK(session.inputs()[0].float32);
  REQUIRE(session.outputs().size() == 1);
  CHECK(session.outputs()[0].name == "y");
  CHECK(session.metadata("spg_fixture") == "scale-by-two");
  CHECK_FALSE(session.metadata("absent").has_value());

  const std::vector<float> input{1.0F, -2.0F, 3.5F, 0.0F, 10.0F, -0.25F};
  CHECK(doubled(session, input, {2, 3}) == times_two(input));
}

TEST_CASE("Default options pick the platform provider and compute the same result", "[runtime]") {
  const Session session = Session::open(shared_env(), fixture("scale_static.onnx"));

  CHECK(session.provider() == expected_default_provider());
  REQUIRE_FALSE(session.attempts().empty());
  CHECK(session.attempts().back().selected);
  CHECK(session.attempts().back().provider == session.provider());

  const std::vector<float> input{0.5F, 1.5F, -3.0F, 4.0F, 8.0F, 16.0F};
  CHECK(doubled(session, input, {2, 3}) == times_two(input));
}

TEST_CASE("A provider missing from the build falls back to CPU and records why", "[runtime]") {
  const auto available = spg::runtime::available_providers();
  std::optional<Provider> missing;
  for (const Provider provider : {Provider::Cuda, Provider::DirectMl, Provider::CoreMl}) {
    if (!contains(available, provider)) {
      missing = provider;
      break;
    }
  }
  if (!missing) {
    SKIP("every accelerated provider is in this ONNX Runtime build");
  }

  SessionOptions options;
  options.providers = {*missing};
  const Session session = Session::open(shared_env(), fixture("scale_static.onnx"), options);
  CHECK(session.provider() == Provider::Cpu);
  REQUIRE(session.attempts().size() == 2);
  CHECK(session.attempts()[0].provider == *missing);
  CHECK_FALSE(session.attempts()[0].selected);
  CHECK_THAT(session.attempts()[0].detail, ContainsSubstring("not in this ONNX Runtime build"));
  CHECK(session.attempts()[1].provider == Provider::Cpu);
  CHECK(session.attempts()[1].selected);

  SECTION("without CPU fallback it is a ProviderUnavailable error naming the provider") {
    options.cpu_fallback = false;
    const std::string message = error_of(ErrorCode::ProviderUnavailable, [&] {
      static_cast<void>(Session::open(shared_env(), fixture("scale_static.onnx"), options));
    });
    CHECK_THAT(message, ContainsSubstring(std::string(spg::runtime::to_string(*missing))));
    CHECK_THAT(message, ContainsSubstring("CPU fallback off"));
    CHECK_THAT(message, ContainsSubstring("scale_static.onnx"));
  }
}

TEST_CASE("Unusable model files fail with clear errors", "[runtime]") {
  SECTION("a missing file is ModelNotFound and names the path") {
    const std::string message = error_of(ErrorCode::ModelNotFound, [] {
      static_cast<void>(Session::open(shared_env(), fixture("does_not_exist.onnx")));
    });
    CHECK_THAT(message, ContainsSubstring("does_not_exist.onnx"));
    CHECK_THAT(message, ContainsSubstring("fetch_models.py"));
  }
  SECTION("a directory is ModelNotFound") {
    error_of(ErrorCode::ModelNotFound, [] { static_cast<void>(Session::open(shared_env(), fixture(""))); });
  }
  SECTION("a corrupt file is ModelLoadFailed on every provider") {
    const std::string message = error_of(ErrorCode::ModelLoadFailed, [] {
      static_cast<void>(Session::open(shared_env(), fixture("not_a_model.onnx")));
    });
    CHECK_THAT(message, ContainsSubstring("not_a_model.onnx"));
    CHECK_THAT(message, ContainsSubstring("cpu:"));
  }
}

TEST_CASE("Dynamic inputs need explicit warmup shapes", "[runtime]") {
  SessionOptions options = cpu_only();

  SECTION("no shapes is an InvalidInput error naming the input") {
    const std::string message = error_of(ErrorCode::InvalidInput, [&] {
      static_cast<void>(Session::open(shared_env(), fixture("scale_dynamic.onnx"), options));
    });
    CHECK_THAT(message, ContainsSubstring("'x'"));
    CHECK_THAT(message, ContainsSubstring("[?, 4]"));
  }
  SECTION("a shape per input warms up, then any batch size runs") {
    options.warmup_shapes = {{5, 4}};
    options.warmup_runs = 2;
    const Session session = Session::open(shared_env(), fixture("scale_dynamic.onnx"), options);
    CHECK(session.inputs()[0].shape == std::vector<std::int64_t>{-1, 4});
    const std::vector<float> input{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(doubled(session, input, {3, 4}) == times_two(input));
  }
  SECTION("the wrong number of shapes is rejected") {
    options.warmup_shapes = {{1, 4}, {1, 4}};
    error_of(ErrorCode::InvalidInput, [&] {
      static_cast<void>(Session::open(shared_env(), fixture("scale_dynamic.onnx"), options));
    });
  }
  SECTION("a shape that contradicts a fixed dimension is rejected") {
    options.warmup_shapes = {{1, 5}};
    const std::string message = error_of(ErrorCode::InvalidInput, [&] {
      static_cast<void>(Session::open(shared_env(), fixture("scale_dynamic.onnx"), options));
    });
    CHECK_THAT(message, ContainsSubstring("[1, 5]"));
  }
}

TEST_CASE("run() rejects inputs that do not fit and never echoes their values", "[runtime]") {
  const Session session = Session::open(shared_env(), fixture("scale_static.onnx"), cpu_only());
  const std::vector<float> six(6, 12345.5F);
  const std::vector<float> five(5, 12345.5F);
  const std::vector<std::int64_t> good{2, 3};
  const std::vector<std::int64_t> wrong_dim{3, 2};
  const std::vector<std::int64_t> wrong_rank{6};
  const std::vector<std::int64_t> negative{-2, 3};

  std::vector<std::string> messages;
  messages.push_back(error_of(ErrorCode::InvalidInput, [&] { static_cast<void>(session.run({})); }));
  for (const auto& [data, shape] : {std::pair{&six, &wrong_dim}, std::pair{&six, &wrong_rank},
                                    std::pair{&six, &negative}, std::pair{&five, &good}}) {
    const TensorView view{*data, *shape};
    messages.push_back(error_of(ErrorCode::InvalidInput, [&] { static_cast<void>(session.run({&view, 1})); }));
  }
  CHECK_THAT(messages[1], ContainsSubstring("[3, 2]") && ContainsSubstring("[2, 3]"));
  CHECK_THAT(messages[4], ContainsSubstring("needs 6 values, got 5"));
  for (const std::string& message : messages) {
    CHECK_THAT(message, !ContainsSubstring("12345"));
  }
}

TEST_CASE("Concurrent runs on one session stay independent", "[runtime]") {
  const Session session = Session::open(shared_env(), fixture("scale_static.onnx"));
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&session, &wrong, t] {
      const std::vector<std::int64_t> shape{2, 3};
      for (int i = 0; i < 50; ++i) {
        const auto base = static_cast<float>(t * 1000 + i);
        const std::vector<float> input{base, base + 1, base + 2, base + 3, base + 4, base + 5};
        const TensorView view{input, shape};
        if (session.run({&view, 1}).at(0).data != times_two(input)) {
          ++wrong;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CHECK(wrong.load() == 0);
}

TEST_CASE("A moved session keeps working", "[runtime]") {
  Session first = Session::open(shared_env(), fixture("scale_static.onnx"), cpu_only());
  const Session second = std::move(first);
  const std::vector<float> input{1, 2, 3, 4, 5, 6};
  CHECK(doubled(second, input, {2, 3}) == times_two(input));
}

TEST_CASE("Pinned models load, warm up and run", "[runtime][models]") {
  struct Entry {
    std::string name;
    fs::path path;
    std::vector<std::vector<std::int64_t>> shapes;
    std::vector<std::string> metadata;
  };
  std::vector<Entry> entries;
  std::vector<std::string> missing;
  std::ifstream listing(fixture("models.txt"));
  REQUIRE(listing);
  for (std::string line; std::getline(listing, line);) {
    std::istringstream fields(line);
    Entry entry;
    std::string path;
    std::string shapes;
    std::string keys;
    std::getline(fields, entry.name, '\t');
    std::getline(fields, path, '\t');
    std::getline(fields, shapes, '\t');
    std::getline(fields, keys, '\t');
    entry.path = fs::path(std::u8string(path.begin(), path.end()));
    std::istringstream shape_list(shapes);
    for (std::string shape; std::getline(shape_list, shape, ';');) {
      std::istringstream dims(shape);
      entry.shapes.emplace_back();
      for (std::string dim; std::getline(dims, dim, 'x');) {
        entry.shapes.back().push_back(static_cast<std::int64_t>(std::stoll(dim)));
      }
    }
    std::istringstream key_list(keys);
    for (std::string key; std::getline(key_list, key, ',');) {
      entry.metadata.push_back(key);
    }
    if (!fs::is_regular_file(entry.path)) {
      missing.push_back(entry.name);
    }
    entries.push_back(std::move(entry));
  }
  REQUIRE_FALSE(entries.empty());
  if (!missing.empty()) {
    std::string names;
    for (const std::string& name : missing) {
      names += " " + name;
    }
    SKIP("models not in the cache (run scripts/fetch_models.py):" << names);
  }

  for (const Entry& entry : entries) {
    for (const bool accelerated : {true, false}) {
      DYNAMIC_SECTION(entry.name << (accelerated ? " on the default provider" : " on CPU")) {
        SessionOptions options = accelerated ? SessionOptions{} : cpu_only();
        options.warmup_shapes = entry.shapes;
        const auto start = std::chrono::steady_clock::now();
        const Session session = Session::open(shared_env(), entry.path, options);
        const auto opened = std::chrono::steady_clock::now();
        CHECK(session.provider() == (accelerated ? expected_default_provider() : Provider::Cpu));

        std::vector<std::vector<float>> buffers;
        std::vector<TensorView> views;
        buffers.reserve(entry.shapes.size());
        for (const auto& shape : entry.shapes) {
          std::size_t count = 1;
          for (const std::int64_t dim : shape) {
            count *= static_cast<std::size_t>(dim);
          }
          buffers.emplace_back(count, 0.5F);
          views.push_back({buffers.back(), shape});
        }
        const auto run_start = std::chrono::steady_clock::now();
        const std::vector<Tensor> outputs = session.run(views);
        const auto run_end = std::chrono::steady_clock::now();

        CHECK(outputs.size() == session.outputs().size());
        for (const Tensor& output : outputs) {
          CHECK_FALSE(output.data.empty());
          CHECK(std::all_of(output.data.begin(), output.data.end(), [](float v) { return std::isfinite(v); }));
        }
        for (const std::string& key : entry.metadata) {
          const auto value = session.metadata(key);
          REQUIRE(value.has_value());
          CHECK_FALSE(value->empty());
        }
        using ms = std::chrono::duration<double, std::milli>;
        std::cout << "runtime-timing " << entry.name << " provider=" << spg::runtime::to_string(session.provider())
                  << " open+warmup_ms=" << ms(opened - start).count()
                  << " run_ms=" << ms(run_end - run_start).count() << '\n';
      }
    }
  }
}
