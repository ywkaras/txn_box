/** @file
   Per transaction context implementation.

 * Copyright 2019, Oath Inc.
 * SPDX-License-Identifier: Apache-2.0
*/

#include <swoc/MemSpan.h>
#include <swoc/ArenaWriter.h>

#include "txn_box/Context.h"
#include "txn_box/Config.h"
#include "txn_box/Expr.h"
#include "txn_box/ts_util.h"

using swoc::TextView;
using swoc::MemSpan;
using swoc::Errata;
using swoc::BufferWriter;
using swoc::FixedBufferWriter;
using swoc::ArenaWriter;
using namespace swoc::literals;

/* ------------------------------------------------------------------------------------ */
bool Expr::bwf_ex::operator()(std::string_view &literal, Extractor::Spec &spec) {
  bool zret = false;
  if (_iter->_type == swoc::bwf::Spec::LITERAL_TYPE) {
    literal = _iter->_ext;
    if (++_iter == _specs.end()) { // all done!
      return zret;
    }
  }
  if (_iter->_type != swoc::bwf::Spec::LITERAL_TYPE) {
    spec = *_iter;
    ++_iter;
    zret = true;
  }
  return zret;
}
/* ------------------------------------------------------------------------------------ */

Context::Context(std::shared_ptr<Config> const& cfg) : _cfg(cfg) {
  // This is arranged so @a _arena destructor will clean up properly, nothing more need be done.
  _arena.reset(swoc::MemArena::construct_self_contained(4000 + (cfg ? cfg->_ctx_storage_required : 0)));

  if (cfg) {
    _rxp_ctx = pcre2_general_context_create([](PCRE2_SIZE size
                                               , void *ctx) -> void * { return static_cast<self_type *>(ctx)->_arena->alloc(size).data(); }
                                            , [](void *, void *) -> void {}, this);
    // Pre-allocate match data for the maximum # of capture groups in the configuration.
    // This avoids multiple allocations due to the order of which matches are done first.
    _rxp_working._match = pcre2_match_data_create(cfg->_capture_groups, _rxp_ctx);
    _rxp_working._n = cfg->_capture_groups;
    _rxp_active._match = pcre2_match_data_create(cfg->_capture_groups, _rxp_ctx);
    _rxp_active._n = cfg->_capture_groups;

    // Directive shared storage
    _ctx_store = _arena->alloc(cfg->_ctx_storage_required);
  }
}

Context::~Context() {
  // Invoke all the finalizers to do additional cleanup.
  for ( auto && f : _finalizers ) {
    f._f(f._ptr);
    std::destroy_at(&f._f); // clean up the cleaner too, just in case.
  }
}

Errata Context::on_hook_do(Hook hook_idx, Directive *drtv) {
  auto & info { _hooks[IndexFor(hook_idx)] };
  if (! info.hook_set_p) { // no hook to invoke this directive, set one up.
    if (hook_idx >= _cur_hook) {
      TSHttpTxnHookAdd(_txn, TS_Hook[IndexFor(hook_idx)], _cont);
      info.hook_set_p = true;
    } else if (hook_idx < _cur_hook) {
      // error condition - should report. Also, should detect this on config load.
    }
  }
  info.cb_list.append(_arena->make<Callback>(drtv));
  return {};
}

Errata Context::invoke_callbacks() {
  // Bit of subtlety here - directives / callbacks can be added to the list due to the action of the
  // invoked directive from this list. However, because this is an intrusive list and items are only
  // added to the end, the @c next pointer for the current item will be updated before the loop
  // iteration occurs and therefore new directives will be invoked.
  auto & info { _hooks[IndexFor(_cur_hook)] };
  for ( auto & cb : info.cb_list ) {
    _terminal_p = false; // reset before each scheduled callback.
    cb.invoke(*this);
  }
  return {};
}

Errata Context::invoke_for_hook(Hook hook) {
  _cur_hook = hook;
  this->clear_cache();

  // Run the top level directives in the config first.
  if (_cfg) {
    for (auto const &handle : _cfg->hook_directives(hook)) {
      _terminal_p = false; // reset before each top level invocation.
      handle->invoke(*this); // need to log errors here.
    }
  }
  this->invoke_callbacks();

  _cur_hook = Hook::INVALID;

  return {};
}

Errata Context::invoke_for_remap(Config &rule_cfg, TSRemapRequestInfo *rri) {
  _cur_hook = Hook::REMAP;
  _remap_info = rri;
  this->clear_cache();
  // Ugly, but need to make sure the regular expression storage is sufficient for both working
  // and committed match data.
  this->rxp_match_require(rule_cfg._capture_groups);
  this->rxp_commit_match(""); // swap
  this->rxp_match_require(rule_cfg._capture_groups);

  // What about directive storage?

  // Remap rule directives.
  _terminal_p = false; // reset before each top level invocation.
  for (auto const &handle : rule_cfg.hook_directives(_cur_hook)) {
    handle->invoke(*this); // need to log errors here.
    if (_terminal_p) {
      break;
    }
  }
  // Now the global config directives for REMAP
  if (_cfg) {
    for (auto const &handle : _cfg->hook_directives(_cur_hook)) {
      _terminal_p = false; // reset before each top level invocation.
      handle->invoke(*this); // need to log errors here.
    }
  }
  this->invoke_callbacks(); // Any accumulated callbacks.

  // Revert from remap style invocation.
  _cur_hook = Hook::INVALID;
  _remap_info = nullptr;

  return {};
}

void Context::operator()(swoc::BufferWriter& w, Extractor::Spec const& spec) {
  spec._exf->format(w, spec, *this);
}

Feature Expr::bwf_visitor::operator()(const Composite &comp) {
  ArenaWriter w{*_ctx._arena};
  w.print_nfv(_ctx, bwf_ex{comp._specs}, Context::ArgPack(_ctx));
  return w.view();
}

Feature Expr::bwf_visitor::operator()(List const & list) {
  feature_type_for<TUPLE> expr_tuple = _ctx.span<Feature>(list._exprs.size());
  unsigned idx = 0;
  for ( auto const& expr : list._exprs ) {
    Feature feature { _ctx.extract(expr) };
    _ctx.commit(feature);
    expr_tuple[idx++] = feature;
  }
  return expr_tuple;
}

Feature Context::extract(Expr const &expr) {
  auto value = std::visit(Expr::bwf_visitor(*this), expr._expr);
  for ( auto const& mod : expr._mods) {
    value = (*mod)(*this, value);
  }
  return value;
}

FeatureView Context::extract_view(const Expr& expr, std::initializer_list<ViewOption> opts) {
  FeatureView zret;

  bool commit_p = false;
  bool cstr_p = false;
  for ( auto opt : opts ) {
    switch (opt) {
      case EX_COMMIT: commit_p = true; break;
      case EX_C_STR: cstr_p = true; break;
    }
  }

  auto f = this->extract(expr);
  if (IndexFor(STRING) == f.index()) {
    auto view = std::get<IndexFor(STRING)>(f);
    if (cstr_p && ! view._cstr_p) {
      if (! view._literal_p && ! view._direct_p) { // in temporary memory
        // If there's room, just add the null terminator.
        if (auto span = _arena->remnant().rebind<char>(); span.data() == view.data_end()) {
          _arena->alloc(1);
          span[0] = '\0';
          view._cstr_p = true;
        } else {
          _arena->alloc(view.size()); // commit the view data and copy.
          view._literal_p = true;
        }
      }
      // if it's still in fixed memory, need to copy and terminate.
      if (view._literal_p) {
        auto span = _arena->require(view.size() + 1).remnant().rebind<char>();
        memcpy(span, view);
        span[view.size()] = '\0';
        view = span.view();
        view.remove_suffix(1); // drop null from view.
        view._cstr_p = true;
        view._literal_p = false;
      }
    }
    zret = view;
  } else {
    ArenaWriter w{*_arena};
    if (cstr_p) {
      w.print("{}\0", f);
      zret = TextView{w.view()}.remove_suffix(1);
      zret._cstr_p = true;
    } else {
      w.print("{}", f);
      zret = w.view();
    }
  }
  if (commit_p && ! zret._literal_p && ! zret._direct_p) {
    _arena->alloc(zret.size() + (zret._cstr_p ? 1 : 0));
    zret._literal_p = true;
  }
  return zret;
}

Feature& Context::commit(Feature &feature) {
  if (auto fv = std::get_if<STRING>(&feature) ; fv != nullptr) {
    if (fv->_literal_p) {
      // nothing
    } else if (fv->_direct_p) {
      auto span { _arena->alloc(fv->size())};
      memcpy(span, *fv);
      fv->_direct_p = false;
      fv->_literal_p = true;
      *fv = span.view(); // update full to be the localized copy.
    } else if (fv->data() == _arena->remnant().data()) { // it's in transient memory, finalize it.
      _arena->alloc(fv->size());
      fv->_literal_p = true;
    }
  }
  return feature;
}

swoc::MemSpan<void> Context::storage_for(Directive const * drtv) {
  return this->storage_for(drtv->_rtti);
}

swoc::MemSpan<void> Context::storage_for(Directive::CfgInfo const * rtti) {
  auto zret { _ctx_store };
  zret.remove_prefix(rtti->_ctx_storage_offset);
  zret.remove_suffix(zret.size() - rtti->_ctx_storage_offset);
  return zret;
}

ts::HttpRequest Context::ua_req_hdr() {
  if (!_ua_req.is_valid()) {
    _ua_req = _txn.ua_req_hdr();
  }
  return _ua_req;
}

ts::HttpRequest Context::proxy_req_hdr() {
  if (!_proxy_req.is_valid()) {
    _proxy_req = _txn.preq_hdr();
  }
  return _proxy_req;
}

ts::HttpResponse Context::upstream_rsp_hdr() {
  if (!_upstream_rsp.is_valid()) {
    _upstream_rsp = _txn.ursp_hdr();
  }
  return _upstream_rsp;
}

ts::HttpResponse Context::proxy_rsp_hdr() {
  if (!_proxy_rsp.is_valid()) {
    _proxy_rsp = _txn.prsp_hdr();
  }
  return _proxy_rsp;
}

Context::self_type &Context::enable_hooks(TSHttpTxn txn) {
  // Create a continuation to hold the data.
  _cont = TSContCreate(ts_callback, TSContMutexGet(reinterpret_cast<TSCont>(txn)));
  TSContDataSet(_cont, this);
  _txn = txn;

  // set hooks for top level directives.
  if (_cfg) {
    for (unsigned idx = 0; idx < std::tuple_size<Hook>::value; ++idx) {
      auto const &drtv_list{_cfg->hook_directives(static_cast<Hook>(idx))};
      if (!drtv_list.empty()) {
        TSHttpTxnHookAdd(txn, TS_Hook[idx], _cont);
        _hooks[idx].hook_set_p = true;
      }
    }
  }

  // Always set a cleanup hook.
  TSHttpTxnHookAdd(txn, TS_HTTP_TXN_CLOSE_HOOK, _cont);
  _txn.arg_assign(G.TxnArgIdx, this);
  return *this;
}

int Context::ts_callback(TSCont cont, TSEvent evt, void *) {
  self_type * self = static_cast<self_type*>(TSContDataGet(cont));
  auto txn = self->_txn; // cache in case it's a close.

  // Run the directives.
  Hook hook { Convert_TS_Event_To_TxB_Hook(evt) };
  if (Hook::INVALID != hook) {
    self->invoke_for_hook(hook);
  }

  /// TXN Close is special
  if (TS_EVENT_HTTP_TXN_CLOSE == evt) {
    TSContDataSet(cont, nullptr);
    TSContDestroy(cont);
    delete self;
  }

  TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
  return TS_SUCCESS;
}

Context & Context::rxp_match_require(unsigned n) {
  if (_rxp_working._n < n) {
    // Bump up at least 7, or 50%, or at least @a n.
    n = std::max(_rxp_working._n + 7, n);
    n = std::max(3 * _rxp_working._n / 2 , n);
    _rxp_working._match = pcre2_match_data_create(n, _rxp_ctx);
    _rxp_working._n = n;
  }
  return *this;
}

void Context::set_literal_capture(swoc::TextView text) {
  auto ovector = pcre2_get_ovector_pointer(_rxp_active._match);
  ovector[0] = 0;
  ovector[1] = text.size()-1;
  _rxp_src = text;
}

Context::RxpCapture *Context::rxp_commit_match(swoc::TextView const&src) {
  _rxp_src = src;
  std::swap(_rxp_active, _rxp_working);
  return &_rxp_active;
}

Feature const&Context::load_txn_var(swoc::TextView const&name) {
  auto spot = _txn_vars.find(name);
  if (spot == _txn_vars.end()) {
    // Later, need to search ssn and global variables and retrieve those if found.
    return NIL_FEATURE;
  }
  return spot->_value;
}

Context::self_type&Context::store_txn_var(swoc::TextView const&name, Feature&value) {
  auto spot = _txn_vars.find(name);
  this->commit(value);
  if (spot == _txn_vars.end()) {
    _txn_vars.insert(_arena->make<TxnVar>(name, value));
  } else {
    spot->_value = value;
  }
  return *this;
}

TextView Context::localize_as_c_str(swoc::TextView text) {
  // If it's empty or isn't already a C string, make a copy that is.
  if (text.empty() || '\0' != text.back()) {
    auto span = _arena->alloc_span<char>(text.size() + 1);
    memcpy(span, text);
    span[text.size()] = '\0';
    text = span.view();
  }
  return text;
}

unsigned Context::ArgPack::count() const {
  return pcre2_get_ovector_count(_ctx._rxp_active._match);
}

BufferWriter& Context::ArgPack::print(BufferWriter &w
                                      , swoc::bwf::Spec const &spec, unsigned idx) const {
  auto ovector = pcre2_get_ovector_pointer(_ctx._rxp_active._match);
  idx *= 2; // To account for offset pairs.
  return bwformat(w, spec, _ctx._rxp_src.substr(ovector[idx], ovector[idx+1] - ovector[idx]));
}

std::any Context::ArgPack::capture(unsigned) const { return "Bogus"_sv; }
