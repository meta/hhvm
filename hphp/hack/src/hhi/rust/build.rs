use std::path::Path;
use std::path::PathBuf;

fn main() {
    const ROOT_PATH: &str = "../../../../..";

    let root_path = Path::new(ROOT_PATH);
    let hhi_lib = root_path.join("hphp/hack/hhi");
    let hsl = root_path.join("hphp/hsl");

    let out_dir = std::env::var("OUT_DIR").unwrap();
    let out_dir = Path::new(&out_dir);

    let mut contents: Vec<(PathBuf, String)> = Vec::new();

    for src in [hhi_lib, hsl] {
        for entry in walkdir::WalkDir::new(src) {
            let entry = entry.unwrap();
            if !entry.file_type().is_file() {
                continue;
            }
            let path = entry.path();
            let ext = path.extension().unwrap_or(std::ffi::OsStr::new(""));
            if ext != "php" && ext != "hack" {
                continue;
            }
            let source_path = path.strip_prefix(root_path).ok().and_then(Path::to_str);
            let mut out: Vec<u8> = Vec::new();
            if generate_hhi_lib::run(&mut out, path, source_path).is_ok() {
                rerun_if_changed(path);
                contents.push((path.to_path_buf(), String::from_utf8(out).unwrap()));
            }
        }
    }

    gen_hhi_contents_lib::write_hhi_contents_file(&out_dir.join("hhi_contents.rs"), &contents)
        .unwrap();

    rerun_if_changed("build.rs");
    rerun_if_changed("gen_hhi_contents.rs");
}

fn rerun_if_changed<P: AsRef<Path>>(f: P) {
    println!("cargo:rerun-if-changed={}", f.as_ref().to_str().unwrap());
}
