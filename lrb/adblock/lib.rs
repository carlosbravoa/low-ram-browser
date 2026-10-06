// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! lrb's bridge to adblock-rust (//third_party/rust/lrb/adblock).
//!
//! The engine is used in place from a read-only mapped file shared by every
//! instance (patches/adblock-rust/0001): each instance pays ~0.8 MB, not the
//! 7.8 MB a private copy would cost. Registrable domains come from
//! Chromium's public suffix list, not adblock-rust's own copy.

use std::sync::Once;

#[cxx::bridge(namespace = "lrb::adblock")]
mod ffi {
    struct CheckResult {
        /// Block the request (a blocking rule matched and no exception did,
        /// or an $important rule matched).
        block: bool,
        /// If not empty, answer with this resource instead of blocking
        /// visibly ($redirect rules: e.g. a no-op script).
        redirect: String,
    }

    extern "Rust" {
        type Engine;

        /// Loads the engine from `data`, without copying it. Check `ok()`:
        /// Chromium builds without C++ exceptions, so errors aren't thrown.
        ///
        /// # Safety
        /// `data` must stay mapped and unchanged for the rest of the process.
        unsafe fn load_engine(data: &[u8]) -> Box<Engine>;

        /// Compiles filter lists (EasyList syntax) into an engine file:
        /// lrb's container (magic, offset, padding) around the serialized
        /// engine, laid out so `load_engine` can use it in place.
        /// `network_only` drops cosmetic rules (the lean setting).
        /// `trusted[i]` marks list i as trusted: only trusted lists may use
        /// scriptlets that need it (uBlock Origin trusts its own lists).
        fn compile_engine_file(
            lists: &Vec<String>,
            trusted: &Vec<bool>,
            network_only: bool,
        ) -> Vec<u8>;

        /// A user stylesheet hiding the elements the cosmetic rules for
        /// `url`'s site match (one rule per selector, so an invalid
        /// selector can't void the others). Empty if none.
        fn cosmetic_css(self: &Engine, url: &str) -> String;

        /// The scriptlets to run in `url`'s main world at document start
        /// (uBlock Origin's, e.g. defeating anti-adblock checks), as one
        /// script. Empty if none.
        fn injected_script(self: &Engine, url: &str) -> String;

        /// The file names uBlock Origin's `redirect-resources.js` lists (the
        /// surrogates in its web_accessible_resources directory).
        fn redirect_resource_names(map: &str) -> Vec<String>;

        /// Assembles the surrogates in `dir` (every file named by the map at
        /// `map_path` must be there) into JSON for `use_resources_json`.
        fn assemble_resources_json(dir: &str, map_path: &str) -> String;

        /// Adds surrogates (from `assemble_resources_json`) so $redirect
        /// rules can answer with them. Before the engine is shared.
        fn use_resources_json(self: &mut Engine, json: &str) -> bool;

        /// Whether `load_engine` succeeded; `error()` says why not.
        fn ok(self: &Engine) -> bool;
        fn error(self: &Engine) -> String;

        /// Checks one request. `request_type` uses adblock-rust's names
        /// ("script", "image", "stylesheet", "sub_frame", "xmlhttprequest", ...).
        fn check(
            self: &Engine,
            url: &str,
            source_url: &str,
            request_type: &str,
            method: &str,
        ) -> CheckResult;
    }

    unsafe extern "C++" {
        include!("lrb/adblock/domain_resolver.h");

        /// Start and end of the registrable domain (eTLD+1) within `host`;
        /// (0, host.len()) when there is none.
        fn registrable_domain_bounds(host: &str, start: &mut usize, end: &mut usize);
    }
}

/// The engine file's container: magic, little-endian u64 offset of the
/// serialized engine, padding. Must match lrb/common/content_blocker.cc.
const FILE_MAGIC: &[u8; 8] = b"LRBADB01";

/// The permission bit of trusted lists; scriptlets that need trust carry it
/// (lrb/browser/list_updater.cc's scriptlet conversion sets `permission: 1`).
const TRUSTED: u8 = 1;

fn compile_engine_file(lists: &Vec<String>, trusted: &Vec<bool>, network_only: bool) -> Vec<u8> {
    let mut set = adblock::lists::FilterSet::new(false);
    for (i, list) in lists.iter().enumerate() {
        let opts = adblock::lists::ParseOptions {
            rule_types: if network_only {
                adblock::lists::RuleTypes::NetworkOnly
            } else {
                adblock::lists::RuleTypes::All
            },
            permissions: adblock::resources::PermissionMask::from_bits(
                if trusted.get(i).copied().unwrap_or(false) { TRUSTED } else { 0 },
            ),
            ..Default::default()
        };
        set.add_filter_list(list.clone(), opts);
    }
    let dat = adblock::Engine::new_with_filter_set(set).serialize();
    // The engine data after adblock-rust's header must be 8-byte aligned in
    // a page-aligned mapping.
    let header = adblock::Engine::serialized_header_length();
    let mut offset = 16;
    while (offset + header) % 8 != 0 {
        offset += 1;
    }
    let mut file = Vec::with_capacity(offset + dat.len());
    file.extend_from_slice(FILE_MAGIC);
    file.extend_from_slice(&(offset as u64).to_le_bytes());
    file.resize(offset, 0);
    file.extend_from_slice(&dat);
    file
}

fn redirect_resource_names(map: &str) -> Vec<String> {
    // Top-level entries of `new Map([ [ 'name', { ... } ], ... ])`.
    let entry = regex::Regex::new(r"(?m)^\s*\[\s*'([^']+)'\s*,").unwrap();
    entry
        .captures_iter(map)
        .map(|c| c[1].to_string())
        .filter(|name| !name.contains("..") && !name.starts_with('/'))
        .collect()
}

fn assemble_resources_json(dir: &str, map_path: &str) -> String {
    let resources = adblock::resources::resource_assembler::assemble_web_accessible_resources(
        std::path::Path::new(dir),
        std::path::Path::new(map_path),
    );
    serde_json::to_string(&resources).unwrap_or_else(|_| "[]".to_string())
}

pub struct Engine {
    engine: Option<adblock::Engine>,
    error: String,
}

struct ChromiumDomainResolver;

impl adblock::url_parser::ResolvesDomain for ChromiumDomainResolver {
    fn get_host_domain(&self, host: &str) -> (usize, usize) {
        let (mut start, mut end) = (0usize, host.len());
        ffi::registrable_domain_bounds(host, &mut start, &mut end);
        (start, end)
    }
}

unsafe fn load_engine(data: &[u8]) -> Box<Engine> {
    static RESOLVER: Once = Once::new();
    RESOLVER.call_once(|| {
        let _ = adblock::url_parser::set_domain_resolver(Box::new(ChromiumDomainResolver));
    });
    // SAFETY: the caller keeps `data` mapped for the life of the process.
    let data: &'static [u8] = unsafe { std::mem::transmute::<&[u8], &'static [u8]>(data) };
    let mut engine = adblock::Engine::default();
    Box::new(match engine.deserialize_static(data) {
        Ok(()) => Engine { engine: Some(engine), error: String::new() },
        Err(e) => Engine { engine: None, error: format!("{e:?}") },
    })
}

impl Engine {
    fn use_resources_json(&mut self, json: &str) -> bool {
        let Some(engine) = &mut self.engine else { return false };
        match serde_json::from_str::<Vec<adblock::resources::Resource>>(json) {
            Ok(resources) => {
                engine.use_resources(resources);
                true
            }
            Err(_) => false,
        }
    }

    fn cosmetic_css(&self, url: &str) -> String {
        let Some(engine) = &self.engine else { return String::new() };
        let resources = engine.url_cosmetic_resources(url);
        let mut css = String::new();
        for selector in &resources.hide_selectors {
            css.push_str(selector);
            css.push_str("{display:none!important}\n");
        }
        css
    }

    fn injected_script(&self, url: &str) -> String {
        let Some(engine) = &self.engine else { return String::new() };
        engine.url_cosmetic_resources(url).injected_script
    }

    fn ok(&self) -> bool {
        self.engine.is_some()
    }

    fn error(&self) -> String {
        self.error.clone()
    }

    fn check(
        &self,
        url: &str,
        source_url: &str,
        request_type: &str,
        method: &str,
    ) -> ffi::CheckResult {
        let Ok(request) = adblock::request::Request::new(url, source_url, request_type, method)
        else {
            return ffi::CheckResult { block: false, redirect: String::new() };
        };
        let Some(engine) = &self.engine else {
            return ffi::CheckResult { block: false, redirect: String::new() };
        };
        let result = engine.check_network_request(&request);
        ffi::CheckResult {
            block: result.should_block(),
            redirect: result.redirect.unwrap_or_default(),
        }
    }
}
