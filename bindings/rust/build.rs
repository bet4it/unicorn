use build_helper::rustc::link_lib;

fn main() {
    println!("cargo:rerun-if-changed=unicorn");
    link_lib(Some(build_helper::LibKind::DyLib), "unicorn");
}
