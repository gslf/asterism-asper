/*
 * llama_guard.cpp — exception-guarded wrappers around llama.cpp C entry
 * points. See llama_guard.h for the rationale. This is deliberately the
 * only C++ translation unit in libasper.
 *
 * MIT License — per aspera ad astra.
 */
#include "llama_guard.h"

#include <limits>

namespace {

struct llg_grammar {
  llama_sampler *inner;
  const llama_vocab *vocab;
  bool failed;
};

const char *llg_grammar_name(const llama_sampler *) {
  return "asper-guarded-grammar";
}

void llg_grammar_eog_only(llg_grammar *guard,
                          llama_token_data_array *candidates) {
  candidates->selected = -1;
  candidates->sorted = false;
  for (size_t i = 0; i < candidates->size; ++i) {
    if (!llama_vocab_is_eog(guard->vocab, candidates->data[i].id)) {
      candidates->data[i].logit =
          -std::numeric_limits<float>::infinity();
    }
  }
}

void llg_grammar_accept(llama_sampler *sampler, llama_token token) {
  auto *guard = static_cast<llg_grammar *>(sampler->ctx);
  if (guard->failed || llama_vocab_is_eog(guard->vocab, token)) {
    return;
  }
  try {
    llama_sampler_accept(guard->inner, token);
  } catch (...) {
    guard->failed = true;
  }
}

void llg_grammar_apply(llama_sampler *sampler,
                       llama_token_data_array *candidates) {
  auto *guard = static_cast<llg_grammar *>(sampler->ctx);
  if (guard->failed) {
    llg_grammar_eog_only(guard, candidates);
    return;
  }
  try {
    llama_sampler_apply(guard->inner, candidates);
  } catch (...) {
    guard->failed = true;
    llg_grammar_eog_only(guard, candidates);
  }
}

void llg_grammar_reset(llama_sampler *sampler) {
  auto *guard = static_cast<llg_grammar *>(sampler->ctx);
  guard->failed = false;
  try {
    llama_sampler_reset(guard->inner);
  } catch (...) {
    guard->failed = true;
  }
}

void llg_grammar_free(llama_sampler *sampler) {
  auto *guard = static_cast<llg_grammar *>(sampler->ctx);
  try {
    llama_sampler_free(guard->inner);
  } catch (...) {
  }
  delete guard;
}

llama_sampler_i llg_grammar_iface = {
    llg_grammar_name,
    llg_grammar_accept,
    llg_grammar_apply,
    llg_grammar_reset,
    nullptr,
    llg_grammar_free,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

extern "C" llama_sampler *asper_llg_sampler_init_grammar(
    const llama_vocab *vocab, const char *grammar_str,
    const char *grammar_root) {
  llama_sampler *inner = nullptr;
  llg_grammar *guard = nullptr;
  try {
    inner = llama_sampler_init_grammar(vocab, grammar_str, grammar_root);
    if (inner == nullptr) {
      return nullptr;
    }
    guard = new llg_grammar{inner, vocab, false};
    llama_sampler *result = llama_sampler_init(&llg_grammar_iface, guard);
    if (result == nullptr) {
      delete guard;
      llama_sampler_free(inner);
    }
    return result;
  } catch (...) {
  }
  delete guard;
  if (inner != nullptr) {
    try {
      llama_sampler_free(inner);
    } catch (...) {
    }
  }
  return nullptr;
}

extern "C" int32_t asper_llg_tokenize(const struct llama_vocab *vocab,
                                      const char *text, int32_t text_len,
                                      llama_token *tokens,
                                      int32_t n_tokens_max, bool add_special,
                                      bool parse_special) {
  try {
    return llama_tokenize(vocab, text, text_len, tokens, n_tokens_max,
                          add_special, parse_special);
  } catch (...) {
  }
  return INT32_MIN;
}

extern "C" int32_t asper_llg_chat_apply_template(
    const char *tmpl, const struct llama_chat_message *chat, size_t n_msg,
    bool add_ass, char *buf, int32_t length) {
  try {
    return llama_chat_apply_template(tmpl, chat, n_msg, add_ass, buf, length);
  } catch (...) {
  }
  return -1;
}

extern "C" int asper_llg_sampler_sample(struct llama_sampler *smpl,
                                        struct llama_context *ctx,
                                        int32_t idx, llama_token *out) {
  try {
    *out = llama_sampler_sample(smpl, ctx, idx);
    return 0;
  } catch (...) {
  }
  return -1;
}

extern "C" int32_t asper_llg_decode(struct llama_context *ctx,
                                    struct llama_batch batch) {
  try {
    return llama_decode(ctx, batch);
  } catch (...) {
  }
  return INT32_MIN;
}

extern "C" int32_t asper_llg_encode(struct llama_context *ctx,
                                    struct llama_batch batch) {
  try {
    return llama_encode(ctx, batch);
  } catch (...) {
  }
  return INT32_MIN;
}

extern "C" void asper_llg_memory_clear(llama_memory_t mem, bool data) {
  try {
    llama_memory_clear(mem, data);
  } catch (...) {
  }
}
