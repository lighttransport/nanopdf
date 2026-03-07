// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF Function Evaluator (Types 0, 2, 3, 4)
// Shared between canvas-exporter, thorvg-backend, blend2d-backend.

#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "nanopdf.hh"

namespace nanopdf {
namespace pdfunc {

template<typename T>
inline T clamp_val(const T& v, const T& lo, const T& hi) {
  return (v < lo) ? lo : (hi < v) ? hi : v;
}

// PostScript calculator stack
class PSStack {
public:
  void push(double v) { stack_.push_back(v); }
  double pop() {
    if (stack_.empty()) return 0.0;
    double v = stack_.back();
    stack_.pop_back();
    return v;
  }
  double top() const { return stack_.empty() ? 0.0 : stack_.back(); }
  size_t size() const { return stack_.size(); }
  bool empty() const { return stack_.empty(); }

  void dup() { if (!stack_.empty()) push(top()); }
  void exch() {
    if (stack_.size() >= 2) {
      double a = pop(), b = pop();
      push(a); push(b);
    }
  }
  void roll(int n, int j) {
    if (n <= 0 || static_cast<size_t>(n) > stack_.size()) return;
    j = ((j % n) + n) % n;
    if (j == 0) return;
    std::vector<double> temp;
    for (int i = 0; i < n; ++i) temp.push_back(pop());
    for (int i = 0; i < n; ++i) push(temp[(i + j) % n]);
  }
  void index(int i) {
    if (i >= 0 && static_cast<size_t>(i) < stack_.size())
      push(stack_[stack_.size() - 1 - i]);
  }
  void copy(int n) {
    if (n <= 0 || static_cast<size_t>(n) > stack_.size()) return;
    size_t start = stack_.size() - n;
    for (int i = 0; i < n; ++i) push(stack_[start + i]);
  }
  const std::vector<double>& data() const { return stack_; }

private:
  std::vector<double> stack_;
};

// Tokenize PostScript code
inline std::vector<std::string> tokenize_postscript(const std::string& code) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < code.size()) {
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t' ||
                                code[i] == '\n' || code[i] == '\r')) ++i;
    if (i >= code.size()) break;
    if (code[i] == '%') {
      while (i < code.size() && code[i] != '\n' && code[i] != '\r') ++i;
      continue;
    }
    if (code[i] == '{' || code[i] == '}') {
      tokens.push_back(std::string(1, code[i]));
      ++i;
      continue;
    }
    std::string token;
    while (i < code.size() && code[i] != ' ' && code[i] != '\t' &&
           code[i] != '\n' && code[i] != '\r' &&
           code[i] != '{' && code[i] != '}' && code[i] != '%') {
      token += code[i]; ++i;
    }
    if (!token.empty()) tokens.push_back(token);
  }
  return tokens;
}

inline bool is_ps_number(const std::string& token, double& value) {
  if (token.empty()) return false;
  char* end = nullptr;
  value = std::strtod(token.c_str(), &end);
  return end != token.c_str() && *end == '\0';
}

inline std::vector<std::string> collect_procedure(
    const std::vector<std::string>& tokens, size_t& pos) {
  std::vector<std::string> proc;
  int depth = 1;
  while (pos < tokens.size() && depth > 0) {
    if (tokens[pos] == "{") ++depth;
    else if (tokens[pos] == "}") { --depth; if (depth == 0) { ++pos; break; } }
    proc.push_back(tokens[pos]); ++pos;
  }
  return proc;
}

// Forward decl
inline bool execute_postscript(const std::vector<std::string>& tokens,
                                size_t& pos, PSStack& stack, int depth);

inline bool execute_procedure(const std::vector<std::string>& proc_tokens,
                               PSStack& stack, int depth) {
  size_t pos = 0;
  return execute_postscript(proc_tokens, pos, stack, depth);
}

inline bool execute_postscript(const std::vector<std::string>& tokens,
                                size_t& pos, PSStack& stack, int depth) {
  if (depth > 100) return false;
  std::vector<std::vector<std::string>> proc_stack;

  while (pos < tokens.size()) {
    const std::string& tok = tokens[pos]; ++pos;
    if (tok == "}") return true;
    if (tok == "{") { proc_stack.push_back(collect_procedure(tokens, pos)); continue; }

    double num_val;
    if (is_ps_number(tok, num_val)) { stack.push(num_val); continue; }
    if (tok == "true") { stack.push(1.0); continue; }
    if (tok == "false") { stack.push(0.0); continue; }

    // Arithmetic
    if (tok == "add") { double b = stack.pop(), a = stack.pop(); stack.push(a + b); }
    else if (tok == "sub") { double b = stack.pop(), a = stack.pop(); stack.push(a - b); }
    else if (tok == "mul") { double b = stack.pop(), a = stack.pop(); stack.push(a * b); }
    else if (tok == "div") { double b = stack.pop(), a = stack.pop(); stack.push(b != 0 ? a / b : 0.0); }
    else if (tok == "idiv") { int b = (int)stack.pop(), a = (int)stack.pop(); stack.push(b != 0 ? (double)(a / b) : 0.0); }
    else if (tok == "mod") { int b = (int)stack.pop(), a = (int)stack.pop(); stack.push(b != 0 ? (double)(a % b) : 0.0); }
    else if (tok == "neg") { stack.push(-stack.pop()); }
    else if (tok == "abs") { stack.push(std::fabs(stack.pop())); }
    else if (tok == "ceiling") { stack.push(std::ceil(stack.pop())); }
    else if (tok == "floor") { stack.push(std::floor(stack.pop())); }
    else if (tok == "round") { stack.push(std::round(stack.pop())); }
    else if (tok == "truncate") { stack.push(std::trunc(stack.pop())); }
    else if (tok == "sqrt") { double v = stack.pop(); stack.push(v >= 0 ? std::sqrt(v) : 0.0); }
    else if (tok == "sin") { stack.push(std::sin(stack.pop() * M_PI / 180.0)); }
    else if (tok == "cos") { stack.push(std::cos(stack.pop() * M_PI / 180.0)); }
    else if (tok == "atan") { double x = stack.pop(), y = stack.pop(); stack.push(std::atan2(y, x) * 180.0 / M_PI); }
    else if (tok == "exp") { double e = stack.pop(), b = stack.pop(); stack.push(std::pow(b, e)); }
    else if (tok == "ln") { double v = stack.pop(); stack.push(v > 0 ? std::log(v) : 0.0); }
    else if (tok == "log") { double v = stack.pop(); stack.push(v > 0 ? std::log10(v) : 0.0); }
    // Relational
    else if (tok == "eq") { double b = stack.pop(), a = stack.pop(); stack.push(a == b ? 1.0 : 0.0); }
    else if (tok == "ne") { double b = stack.pop(), a = stack.pop(); stack.push(a != b ? 1.0 : 0.0); }
    else if (tok == "gt") { double b = stack.pop(), a = stack.pop(); stack.push(a > b ? 1.0 : 0.0); }
    else if (tok == "ge") { double b = stack.pop(), a = stack.pop(); stack.push(a >= b ? 1.0 : 0.0); }
    else if (tok == "lt") { double b = stack.pop(), a = stack.pop(); stack.push(a < b ? 1.0 : 0.0); }
    else if (tok == "le") { double b = stack.pop(), a = stack.pop(); stack.push(a <= b ? 1.0 : 0.0); }
    // Boolean
    else if (tok == "and") { int b = (int)stack.pop(), a = (int)stack.pop(); stack.push((double)(a & b)); }
    else if (tok == "or") { int b = (int)stack.pop(), a = (int)stack.pop(); stack.push((double)(a | b)); }
    else if (tok == "xor") { int b = (int)stack.pop(), a = (int)stack.pop(); stack.push((double)(a ^ b)); }
    else if (tok == "not") { stack.push(((int)stack.pop()) == 0 ? 1.0 : 0.0); }
    else if (tok == "bitshift") { int s = (int)stack.pop(), v = (int)stack.pop(); stack.push((double)(s >= 0 ? (v << s) : (v >> (-s)))); }
    // Stack ops
    else if (tok == "dup") { stack.dup(); }
    else if (tok == "exch") { stack.exch(); }
    else if (tok == "pop") { stack.pop(); }
    else if (tok == "copy") { stack.copy((int)stack.pop()); }
    else if (tok == "index") { stack.index((int)stack.pop()); }
    else if (tok == "roll") { int j = (int)stack.pop(), n = (int)stack.pop(); stack.roll(n, j); }
    // Conditional
    else if (tok == "if") {
      bool cond = stack.pop() != 0.0;
      if (!proc_stack.empty()) {
        auto proc = std::move(proc_stack.back()); proc_stack.pop_back();
        if (cond) execute_procedure(proc, stack, depth + 1);
      }
    } else if (tok == "ifelse") {
      bool cond = stack.pop() != 0.0;
      std::vector<std::string> fp, tp;
      if (proc_stack.size() >= 2) {
        fp = std::move(proc_stack.back()); proc_stack.pop_back();
        tp = std::move(proc_stack.back()); proc_stack.pop_back();
      }
      execute_procedure(cond ? tp : fp, stack, depth + 1);
    }
    else if (tok == "cvr") { /* already real */ }
    else if (tok == "cvi") { stack.push((double)(int)stack.pop()); }
    // Unknown: ignore
  }
  return true;
}

// Evaluate a PostScript Type 4 function
inline bool evaluate_postscript_function(const std::string& code,
                                          const std::vector<double>& inputs,
                                          const std::vector<double>& domain,
                                          const std::vector<double>& range,
                                          std::vector<double>& outputs) {
  std::string stripped = code;
  size_t start = stripped.find('{');
  size_t end = stripped.rfind('}');
  if (start != std::string::npos && end != std::string::npos && end > start)
    stripped = stripped.substr(start + 1, end - start - 1);

  auto tokens = tokenize_postscript(stripped);
  PSStack stack;
  for (size_t i = 0; i < inputs.size(); ++i) {
    double v = inputs[i];
    if (domain.size() >= (i + 1) * 2) v = clamp_val(v, domain[i * 2], domain[i * 2 + 1]);
    stack.push(v);
  }

  size_t pos = 0;
  if (!execute_postscript(tokens, pos, stack, 0)) return false;

  int n_out = static_cast<int>(range.size() / 2);
  if (n_out <= 0) n_out = static_cast<int>(stack.size());
  outputs.clear();
  const auto& sd = stack.data();
  for (int i = n_out - 1; i >= 0 && !sd.empty(); --i) {
    size_t idx = sd.size() - 1 - i;
    if (idx < sd.size()) {
      double v = sd[idx];
      if (range.size() >= static_cast<size_t>((n_out - 1 - i + 1) * 2)) {
        size_t ri = (n_out - 1 - i) * 2;
        v = clamp_val(v, range[ri], range[ri + 1]);
      }
      outputs.push_back(v);
    }
  }
  std::reverse(outputs.begin(), outputs.end());
  return !outputs.empty();
}

// Main entry: evaluate any PDF function (Type 0, 2, 3, 4)
inline bool evaluate(const Pdf& pdf, const Value& function,
                      const std::vector<double>& inputs,
                      std::vector<double>& outputs) {
  const Dictionary* fd = nullptr;
  if (function.type == Value::DICTIONARY) fd = &function.dict;
  else if (function.type == Value::STREAM) fd = &function.stream.dict;
  else return false;

  auto ft_it = fd->find("FunctionType");
  if (ft_it == fd->end() || ft_it->second.type != Value::NUMBER) return false;
  int func_type = static_cast<int>(ft_it->second.number);

  // Domain
  std::vector<double> domain;
  auto di = fd->find("Domain");
  if (di != fd->end() && di->second.type == Value::ARRAY)
    for (const auto& v : di->second.array)
      if (v.type == Value::NUMBER) domain.push_back(v.number);

  // Range
  std::vector<double> range;
  auto ri = fd->find("Range");
  if (ri != fd->end() && ri->second.type == Value::ARRAY)
    for (const auto& v : ri->second.array)
      if (v.type == Value::NUMBER) range.push_back(v.number);

  if (func_type == 0) {
    // Type 0: Sampled function
    auto size_it = fd->find("Size");
    auto bps_it = fd->find("BitsPerSample");
    if (size_it == fd->end() || bps_it == fd->end()) return false;

    std::vector<int> sizes;
    if (size_it->second.type == Value::ARRAY)
      for (const auto& v : size_it->second.array)
        if (v.type == Value::NUMBER) sizes.push_back(static_cast<int>(v.number));

    int bps = static_cast<int>(bps_it->second.number);
    int n_out = static_cast<int>(range.size() / 2);
    if (n_out <= 0) n_out = 3;

    std::vector<double> decode;
    auto dec_it = fd->find("Decode");
    if (dec_it != fd->end() && dec_it->second.type == Value::ARRAY)
      for (const auto& v : dec_it->second.array)
        if (v.type == Value::NUMBER) decode.push_back(v.number);

    if (function.type == Value::STREAM && !sizes.empty() && !inputs.empty()) {
      auto decoded = decode_stream(pdf, function);
      if (decoded.success && !decoded.data.empty() && sizes.size() == 1 && domain.size() >= 2) {
        double t = (inputs[0] - domain[0]) / (domain[1] - domain[0]);
        t = clamp_val(t, 0.0, 1.0);
        int idx = static_cast<int>(t * (sizes[0] - 1));
        idx = clamp_val(idx, 0, sizes[0] - 1);
        outputs.resize(n_out);
        int bps_bytes = (bps + 7) / 8;
        int off = idx * n_out * bps_bytes;
        for (int i = 0; i < n_out && off + i * bps_bytes < (int)decoded.data.size(); ++i) {
          double val = 0;
          if (bps == 8) val = decoded.data[off + i] / 255.0;
          else if (bps == 16) val = ((decoded.data[off + i * 2] << 8) | decoded.data[off + i * 2 + 1]) / 65535.0;
          if (decode.size() >= (size_t)((i + 1) * 2))
            val = decode[i * 2] + val * (decode[i * 2 + 1] - decode[i * 2]);
          outputs[i] = val;
        }
        return true;
      }
    }
    outputs.resize(n_out, 0.5);
    return true;

  } else if (func_type == 2) {
    // Type 2: Exponential interpolation
    std::vector<double> c0, c1;
    double n_exp = 1.0;
    auto c0i = fd->find("C0");
    if (c0i != fd->end() && c0i->second.type == Value::ARRAY)
      for (const auto& v : c0i->second.array) if (v.type == Value::NUMBER) c0.push_back(v.number);
    auto c1i = fd->find("C1");
    if (c1i != fd->end() && c1i->second.type == Value::ARRAY)
      for (const auto& v : c1i->second.array) if (v.type == Value::NUMBER) c1.push_back(v.number);
    auto ni = fd->find("N");
    if (ni != fd->end() && ni->second.type == Value::NUMBER) n_exp = ni->second.number;
    if (c0.empty()) c0 = {0.0};
    if (c1.empty()) c1 = {1.0};

    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) {
      x = clamp_val(x, domain[0], domain[1]);
      double dr = domain[1] - domain[0];
      if (dr > 0) x = (x - domain[0]) / dr;
    }
    double xp = std::pow(x, n_exp);
    outputs.resize(c0.size());
    for (size_t i = 0; i < c0.size(); ++i)
      outputs[i] = c0[i] + xp * ((i < c1.size() ? c1[i] : 1.0) - c0[i]);
    return true;

  } else if (func_type == 3) {
    // Type 3: Stitching function
    auto fi = fd->find("Functions");
    if (fi == fd->end() || fi->second.type != Value::ARRAY) return false;
    const auto& funcs = fi->second.array;

    std::vector<double> bounds, encode;
    auto bi = fd->find("Bounds");
    if (bi != fd->end() && bi->second.type == Value::ARRAY)
      for (const auto& v : bi->second.array) if (v.type == Value::NUMBER) bounds.push_back(v.number);
    auto ei = fd->find("Encode");
    if (ei != fd->end() && ei->second.type == Value::ARRAY)
      for (const auto& v : ei->second.array) if (v.type == Value::NUMBER) encode.push_back(v.number);

    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) x = clamp_val(x, domain[0], domain[1]);

    size_t fi_idx = 0;
    double sd_min = domain.empty() ? 0.0 : domain[0];
    double sd_max = bounds.empty() ? (domain.size() >= 2 ? domain[1] : 1.0) : bounds[0];
    for (size_t i = 0; i < bounds.size(); ++i) {
      if (x < bounds[i]) { fi_idx = i; sd_max = bounds[i]; break; }
      sd_min = bounds[i]; fi_idx = i + 1;
      sd_max = (i + 1 < bounds.size()) ? bounds[i + 1] : (domain.size() >= 2 ? domain[1] : 1.0);
    }
    if (fi_idx >= funcs.size()) fi_idx = funcs.size() - 1;

    double e_min = (fi_idx * 2 < encode.size()) ? encode[fi_idx * 2] : 0.0;
    double e_max = (fi_idx * 2 + 1 < encode.size()) ? encode[fi_idx * 2 + 1] : 1.0;
    double sub_x = e_min;
    if (sd_max != sd_min) sub_x = e_min + (x - sd_min) / (sd_max - sd_min) * (e_max - e_min);

    Value sub_func = funcs[fi_idx];
    if (sub_func.type == Value::REFERENCE) {
      auto res = resolve_reference(pdf, sub_func.ref_object_number, sub_func.ref_generation_number);
      if (res.success) sub_func = res.value;
    }
    return evaluate(pdf, sub_func, {sub_x}, outputs);

  } else if (func_type == 4) {
    // Type 4: PostScript calculator
    if (function.type != Value::STREAM) return false;
    auto decoded = decode_stream(pdf, function);
    if (!decoded.success || decoded.data.empty()) return false;
    std::string ps_code(decoded.data.begin(), decoded.data.end());
    return evaluate_postscript_function(ps_code, inputs, domain, range, outputs);
  }

  return false;
}

}  // namespace pdfunc
}  // namespace nanopdf
