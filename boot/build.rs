//! Put core1-sram.x on the linker path: memory.x (in this directory, where
//! the linker finds it first; cortex-m-rt's link.x includes it) ends with
//! `INCLUDE core1-sram.x`, which is core1.x with the feature "core1-sram"
//! (core 1's hot code in SRAM) and empty without it.
use std::{env, fs, path::PathBuf};

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let x = if env::var_os("CARGO_FEATURE_CORE1_SRAM").is_some() {
        fs::read_to_string("core1.x").unwrap()
    } else {
        String::from("/* feature \"core1-sram\" off: core 1 runs from flash */\n")
    };
    fs::write(out.join("core1-sram.x"), x).unwrap();
    println!("cargo:rustc-link-search={}", out.display());
    println!("cargo:rerun-if-changed=memory.x");
    println!("cargo:rerun-if-changed=core1.x");
    println!("cargo:rerun-if-changed=build.rs");
}
